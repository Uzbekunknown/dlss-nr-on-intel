"""Full GPU image path parity and fallback/rebuild contracts.

--host-only runs lifecycle/guard tests without touching Vulkan. --full-model
also compares the real model's output bytes/history with the daemon's CPU image
algorithm; device-path calls forbid full image views/downloads. This diagnostic
reads the final result only after process_device has returned.
"""
from __future__ import annotations
import argparse
import contextlib
import os
import pathlib
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"src/ref"))
sys.path.insert(0,str(ROOT/"src/layer"))
import nr_daemon
import nr_frame
import nr_image
import xmxres as X
import gpu_image_pipeline as G
from test_image_stats import FakeRuntime


def settings(**changes):
    values=dict(profile="standard",intensity=1.,detail_strength=1.,colour_strength=1.,
                render_scale=1.,temporal=.8,cut_limit=.18,hold=.4,release=8.,min_extent=128)
    values.update(changes)
    return SimpleNamespace(**values)


class FakeFrame:
    def __init__(self,rt,height,width):
        self._closed=False;self.rt=rt
        self.input=rt.buffer(height*width*16,np.float16)
        self.head=rt.buffer(height*width*(4 if rt.compact_head else 16))
        self.preparations=0;self.runs=0
    def _prepare_scratch(self,*args): self.preparations+=1
    def input_buffer(self): return self.input
    def run_device(self): self.runs+=1;return self.head


class FakeBackend:
    def __init__(self):
        self.runtime=FakeRuntime();self.runtime.input_fp16=True;self.runtime.compact_head=True
        self.frames=[]
        self.runtime.copy=lambda *args:None
    def frame(self,height,width,*,device_image_io=False):
        if not device_image_io: raise AssertionError("pipeline requested a host frame")
        frame=FakeFrame(self.runtime,height,width);self.frames.append(frame);return frame


class FakePreprocessor:
    def __init__(self,*args,**kwargs): self.closed=False
    def record(self,*args,**kwargs): pass
    def close(self): self.closed=True


class FakeResampler:
    def __init__(self,*args,**kwargs): pass
    def record(self,*args): pass


class FakeComposer:
    def __init__(self,rt,*,frame_size,head_size,vk_format,crop,head_row_width,head_channels):
        height,width=frame_size;top,bottom,left,right=crop
        self.encoded=rt.buffer(height*width*4,np.uint8)
        self.neural=rt.buffer((bottom-top)*(right-left)*3)
        self.composed=rt.buffer((bottom-top)*(right-left)*3)
        self.closed=False
    def configure(self,**kwargs): self.controls=kwargs
    def record(self,*args,**kwargs): return self.encoded
    def close(self): self.closed=True


class FakeStats:
    def __init__(self,rt): self.cut=0.;self.bad=0;self.crop=None
    def region(self,raw,width,height,fmt): return self.crop or (0,height,0,width)
    def delta(self,*args,**kwargs): return self.cut
    def nonfinite(self,*args): return self.bad
    def close(self): pass


class HostChecks(unittest.TestCase):
    def setUp(self):
        for name,value in (("GPUImageStats",FakeStats),("GPUImagePreprocessor",FakePreprocessor),
                           ("GPUImageResampler",FakeResampler),("GPUImageComposer",FakeComposer)):
            current=patch.object(G,name,value);current.start();self.addCleanup(current.stop)
        self.backend=FakeBackend();self.pipe=G.GPUImagePipeline(self.backend)
        self.addCleanup(self.pipe.close)
        self.raw=self.backend.runtime.buffer(192*256*4,np.uint8)

    def test_default_daemon_takes_the_host_path(self):
        # Off unless asked, on any driver: without NR_GPU_IMAGE, NR_GPU_TRANSPORT and NR_GPU_ONLY,
        # as by default, a host frame is accepted and reaches the host image path, and the GPU
        # image pipeline is never built. Where the GPU path is not exact (Mesa), this is what
        # its tests hold; its parity is held on Intel's Windows driver.
        class Reached(Exception): pass
        def region(*args): raise Reached
        class Connection:
            def __init__(self,data): self.data=data
            def recv(self,count):
                chunk,self.data=self.data[:count],self.data[count:]
                return chunk
            def sendall(self,data): raise AssertionError("answered before the host path")
        width,height=8,4
        frame=struct.pack("<4I",nr_daemon.MAGIC,width,height,37)+bytes(range(width*height*4))
        args=SimpleNamespace(live=SimpleNamespace(refresh=lambda:None),
                             letterbox=SimpleNamespace(region=region),dump=None,meter=None)
        keep={k:v for k,v in os.environ.items()
              if k not in ("NR_GPU_IMAGE","NR_GPU_TRANSPORT","NR_GPU_ONLY")}
        with patch.dict(os.environ,keep,clear=True),             patch.object(G,"GPUImagePipeline",side_effect=AssertionError("GPU image pipeline built")):
            with self.assertRaises(Reached):
                nr_daemon.process_connection(Connection(frame),None,args)
        self.assertEqual(args.image_transport,"cpu")

    def test_no_history_first_frame_then_cut_and_reset(self):
        live=settings()
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertFalse(self.pipe.last["history"])
        self.assertEqual(self.pipe.history_frames,1)
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertTrue(self.pipe.last["history"])
        self.pipe.stats.cut=.2
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertFalse(self.pipe.last["history"])
        self.assertEqual(self.pipe.history_frames,1)
        self.pipe.stats.cut=0.
        self.pipe.reset_history()
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertFalse(self.pipe.last["history"])
        live.temporal=0.
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertEqual(self.pipe.history_frames,0)

    def test_profile_crop_scale_layout_and_closed_frame_rebuild(self):
        live=settings()
        self.pipe.process_device(self.raw,256,192,37,live)
        initial=self.pipe.frame;initial_prep=self.pipe.prep
        live.profile=next(name for name in nr_frame.PROFILES if name!="standard")
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertIs(self.pipe.frame,initial)
        self.assertFalse(self.pipe.last["history"])
        live.render_scale=.5
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertIsNot(self.pipe.frame,initial)
        self.assertTrue(initial_prep.closed)
        old=self.pipe.frame
        self.backend.runtime.compact_head=False
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertIsNot(self.pipe.frame,old)
        old=self.pipe.frame
        old._closed=True
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertIsNot(self.pipe.frame,old)
        self.pipe.stats.crop=(8,184,0,256)
        self.pipe.process_device(self.raw,256,192,37,live)
        self.assertEqual(self.pipe.last["crop"],(8,184,0,256))
        self.assertFalse(self.pipe.last["history"])

    def test_fallbacks_before_recording_and_nonfinite_drops_history(self):
        live=settings()
        self.assertIn("SDR",self.pipe.supported(256,192,97,live))
        self.assertIn("diagnostics",self.pipe.supported(256,192,37,live,diagnostic=True))
        self.assertIn("masks",self.pipe.supported(256,192,37,live,interface=True))
        self.assertIn("enlarge",self.pipe.supported(160,28,37,settings(render_scale=.4)))
        self.assertIsNone(self.pipe.supported(64,32,37,settings(render_scale=1.)))
        for width,height in ((0,8),(8.5,8),(True,8)):
            with self.assertRaises(ValueError): self.pipe.process_device(self.raw,width,height,37,live)
        live.render_scale=np.nan
        with self.assertRaises(ValueError): self.pipe.process_device(self.raw,256,192,37,live)
        self.assertEqual(self.backend.frames,[])
        live.render_scale=1.
        self.pipe.process_device(self.raw,256,192,37,live)
        self.pipe.stats.bad=1
        with self.assertRaisesRegex(ValueError,"nonfinite"):
            self.pipe.process_device(self.raw,256,192,37,live)
        self.assertEqual(self.pipe.history_frames,0)
        self.assertEqual(self.backend.runtime.aborts,1)
        self.pipe.close();self.pipe.close()
        with self.assertRaisesRegex(ValueError,"closed"):
            self.pipe.process_device(self.raw,256,192,37,live)


def cpu_image_step(backend,letterbox,history,payload,width,height,fmt,live):
    """Same preparation/history/native composition used in process_connection."""
    whole=nr_daemon.decode(payload,width,height,fmt)
    crop=letterbox.region(whole,(width,height,fmt))
    top,bottom,left,right=crop
    colour=whole[top:bottom,left:right]
    ih,iw=colour.shape[:2]
    extent=nr_frame.render_extent(iw,ih,live.render_scale,live.min_extent)
    inner=nr_daemon.resample(colour,(extent[1],extent[0]))
    geometry=nr_frame.network_geometry(inner.shape[1],inner.shape[0],minimum=live.min_extent)
    shot=(width,height,fmt,top,bottom,left,right,live.profile)
    old_inner,old_neural,previous=history.take(shot,inner,live.cut_limit if live.temporal>0 else -1.)
    features=nr_frame.build_features(inner,geometry=geometry,history=old_inner,
                                     **nr_frame.PROFILES[live.profile])
    head=geometry.crop(backend.run_features(features))
    channels=4 if old_neural is not None else 3
    neural=np.empty_like(colour) if live.temporal>0 else None
    grade=nr_frame.grade_for(**nr_frame.PROFILES[live.profile])
    encoded=np.frombuffer(bytearray(payload),np.uint8).reshape(height,width,4)
    fused=nr_frame.compose_encode(head[...,:channels],colour,encoded,top=top,left=left,
        bgra=nr_daemon.FORMATS[fmt][0]=="bgra8",intensity=live.intensity,
        history=old_neural,history_confidence=live.temporal,history_previous=previous,
        history_hold=live.hold,history_release=live.release,grade=grade,neural=neural)
    if fused is None: raise AssertionError("full-model parity needs native image library")
    if live.temporal>0: history.keep(shot,neural,inner,colour)
    return encoded.tobytes(),neural,crop,history.cut,old_neural is not None


class FullModelChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not nr_frame.WEIGHTS.is_file() or nr_image.library() is None:
            raise RuntimeError("full model/native library is required for --full-model")
        if not X.image_path_exact():
            raise unittest.SkipTest(X.IMAGE_PATH_EXACT_ONLY)
        cls.backend=nr_frame.ResidentBackend(max_cached_frames=4)

    @classmethod
    def tearDownClass(cls): cls.backend.close()

    def test_exact_wire_and_history_while_forbidding_image_readbacks(self):
        rng=np.random.default_rng(735)
        pipe=G.GPUImagePipeline(self.backend)
        self.addCleanup(pipe.close)
        box,history=nr_daemon.Letterbox(),nr_daemon.History()
        live=settings(render_scale=.5)
        alternative=next(name for name in nr_frame.PROFILES if name!="standard")
        scenes={}
        sequence=(
            (96,160,37,8,88,{},0),
            (96,160,37,8,88,{},0),
            (96,160,37,8,88,{},1),
            (96,160,37,8,88,{"profile":alternative,"intensity":1.7},1),
            (112,176,44,8,104,{"profile":"standard"},0),
            (112,176,44,8,104,{"render_scale":1.0},0),
            (112,176,50,0,112,{},0),
            (112,176,50,0,112,{"temporal":0.},0),
        )
        original_download=X.Buffer.download
        readbacks=[]
        def reduced_only(buffer,*args,**kwargs):
            if buffer.kind!=X.HOST_READ:
                raise AssertionError("process_device downloaded a full image tensor")
            count=kwargs.get("count",args[1] if len(args)>1 else None)
            if count is None: raise AssertionError("metadata download must state its reduced count")
            readbacks.append(int(count))
            return original_download(buffer,*args,**kwargs)
        for turn,(height,width,fmt,top,bottom,changes,scene) in enumerate(sequence):
            for name,value in changes.items(): setattr(live,name,value)
            key=(height,width,fmt,top,bottom,scene)
            if key not in scenes:
                source=rng.integers(3,250,(height,width,4),dtype=np.uint8)
                if scene: source[...,:3]=255-source[...,:3]
                source[:top,:,:3]=0;source[bottom:,:,:3]=2
                scenes[key]=source
            raw=scenes[key].copy()
            payload=raw.tobytes()
            expected,neural,crop,cut,used_history=cpu_image_step(
                self.backend,box,history,payload,width,height,fmt,live)
            device=self.backend.runtime.buffer(raw.size,np.uint8,kind=X.DEVICE).upload(raw)
            try:
                self.assertFalse(device.mapped)
                with patch.object(X.Buffer,"view",side_effect=AssertionError("image host view")),\
                     patch.object(X,"host_view",side_effect=AssertionError("image host view")),\
                     patch.object(X.Buffer,"download",reduced_only),\
                     patch.object(X.Buffer,"download_into",side_effect=AssertionError("image readback")):
                    answer=pipe.process_device(device,width,height,fmt,live)
                actual=answer.download(np.uint8).tobytes()
                self.assertEqual(actual,expected,f"wire parity frame {turn}: {pipe.last}")
                self.assertEqual(pipe.last["crop"],crop)
                self.assertEqual(pipe.last["history"],used_history)
                self.assertAlmostEqual(pipe.last["cut"],cut,places=6)
                self.assertFalse(answer.mapped)
                self.assertFalse(pipe.frame.input_buffer().mapped)
                self.assertFalse(pipe.frame.head_buffer().mapped)
                if neural is not None:
                    got=pipe.previous_neural.download().reshape(neural.shape)
                    np.testing.assert_array_equal(got.view(np.uint32),neural.view(np.uint32),
                                                  err_msg=f"history parity frame {turn}")
            finally: device.free()
        self.assertTrue(readbacks)
        self.assertLessEqual(max(readbacks),max(176+112,((192*128*4)+1023)//1024))
        print(f"full model: {len(sequence)} exact wire/history frames; device calls read only reduced metadata")


if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--host-only",action="store_true")
    parser.add_argument("--full-model",action="store_true")
    args,remainder=parser.parse_known_args()
    if args.host_only and args.full_model: parser.error("--full-model uses Vulkan")
    sys.argv=[sys.argv[0],*remainder]
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(HostChecks)
    if args.full_model: suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(FullModelChecks))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
