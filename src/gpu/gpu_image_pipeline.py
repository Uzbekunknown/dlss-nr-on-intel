"""GPU preparation, inference and composition with only reduced host metadata.

The host transport wrapper uploads raw bytes and downloads final bytes once. process_device
is the same image algorithm for external GPU buffers; its pixels never cross the CPU.
"""
from __future__ import annotations
import os
import time
import numpy as np
import xmxres as X
import nr_frame
from gpu_image_preprocess import GPUImagePreprocessor, GPUImageResampler
from gpu_image_compose import GPUImageComposer, fallback_reason
from gpu_image_history import GPUImageFollower
from gpu_image_stats import GPUImageStats

# The history's buffers trade places at the end of a frame instead of being copied (see
# `_process_device`); NR_HISTORY_SWAP=0 is the comparison, the three copies as before.
HISTORY_SWAP = os.environ.get("NR_HISTORY_SWAP", "1") != "0"
# The change a frame's answer made (`change` in the daemon's frame line): a number for the log,
# and the composition's float32 copy exists only for it. NR_CHANGE_METRIC=0 records neither
# (0.2-0.4 ms of a 1280x720 frame, more at 1920x1080), and the line reads `change off`; the
# answer and the history are the same bytes either way.
CHANGE_METRIC = os.environ.get("NR_CHANGE_METRIC", "1") != "0"


class GPUImagePipeline:
    def __init__(self, backend):
        self.backend, self.rt = backend, backend.runtime
        self.change_metric = CHANGE_METRIC
        self.stats = GPUImageStats(self.rt)
        self.key = None
        self.history_key = None
        self.history_frames = 0
        self.buffers = []
        self.prep = self.compose = None
        self.follower = None   # made at the first frame with motion
        self.raw = None
        self.wire = None
        self.last = {}
        self.frame = None
        self._closed = False

    def supported(self, width, height, vk_format, live, *, interface=False, diagnostic=False):
        if self._closed: return "GPU image pipeline is closed"
        reason = fallback_reason(vk_format=vk_format, detail_strength=live.detail_strength,
                                 colour_strength=live.colour_strength, interface=interface)
        if reason: return reason
        if not self.rt.input_fp16: return "GPU image preparation requires NR_INPUT_FP16=1"
        if diagnostic: return "dump/meter diagnostics use the reference CPU image path"
        if any(not isinstance(v,(int,np.integer)) or isinstance(v,(bool,np.bool_)) or v<1
               for v in (width,height)): return "invalid frame extent"
        if int(width)*int(height)>0xffffffff//16: return "frame exceeds GPU image indexing capacity"
        if live.profile not in nr_frame.PROFILES: return "unknown image profile"
        values=(live.intensity,live.temporal,live.hold,live.release,live.cut_limit,live.render_scale,
                live.min_extent)
        if not np.isfinite(values).all(): return "GPU image settings must be finite"
        if not .05<=live.render_scale<=1: return "render scale must be between 0.05 and 1"
        if not 128<=live.min_extent<=4096: return "minimum network extent must be 128..4096"
        inner_width, inner_height = nr_frame.render_extent(
            int(width), int(height), float(live.render_scale), int(live.min_extent))
        if inner_width > width or inner_height > height:
            return "GPU image preparation cannot enlarge the active image; use the CPU image path"
        return None

    def _buffer(self, count, dtype=np.float32):
        buffer = self.rt.buffer(count, dtype, kind=X.DEVICE)
        self.buffers.append(buffer)
        return buffer

    def _clear(self):
        if self.prep is not None: self.prep.close()
        if self.compose is not None: self.compose.close()
        if self.follower is not None: self.follower.close()
        for buffer in self.buffers: buffer.free()
        self.buffers = []
        self.prep = self.compose = self.follower = None
        self.frame = None  # cached graph lifetime belongs to the backend, not this wrapper
        self.key = self.history_key = None
        self.history_frames = 0

    def reset_history(self):
        """Drop the temporal link when transport/fallback stops using this image path."""
        self.history_key = None
        self.history_frames = 0

    def _configure(self, width, height, fmt, crop, live):
        top,bottom,left,right=crop
        ah,aw=bottom-top,right-left
        iw,ih=nr_frame.render_extent(aw,ah,float(live.render_scale),int(live.min_extent))
        geometry=nr_frame.network_geometry(iw,ih,minimum=int(live.min_extent))
        if iw>aw or ih>ah:
            raise ValueError("GPU image preparation requires an inner extent no larger than the active image")
        key=(width,height,fmt,crop,ih,iw,geometry.network_height,geometry.network_width,
             bool(self.rt.compact_head),bool(self.rt.input_fp16))
        if key!=self.key or getattr(getattr(self,"frame",None),"_closed",False):
            self._clear()
            self.frame=self.backend.frame(geometry.network_height,geometry.network_width,
                                          device_image_io=True)
            self.frame._prepare_scratch(None)
            self.prep=GPUImagePreprocessor(self.rt,width,height,fmt,inner_size=(ih,iw),
                network_size=(geometry.network_height,geometry.network_width),crop=crop)
            self.active=self._buffer(ah*aw*3)
            self.inner=self._buffer(ih*iw*3)
            self.previous_inner=self._buffer(ih*iw*3)
            self.previous_active=self._buffer(ah*aw*3)
            self.previous_neural=self._buffer(ah*aw*3)
            self.history_inner=self._buffer(ih*iw*3)
            self.decode=GPUImageResampler(self.rt,(height,width),(ah,aw),crop=crop,vk_format=fmt)
            self.inner_decode=GPUImageResampler(self.rt,(height,width),(ih,iw),crop=crop,vk_format=fmt)
            self.history_resize=GPUImageResampler(self.rt,(ah,aw),(ih,iw))
            self.compose=GPUImageComposer(self.rt,frame_size=(height,width),head_size=(ih,iw),
                vk_format=fmt,crop=crop,head_row_width=geometry.network_width,
                head_channels=4 if self.rt.compact_head else 16)
            # The composer's neural target trades places with `previous_neural` every frame
            # (the end of `_process_device`), so either may be the composer's when this
            # pipeline is cleared: both are freed with the pipeline's own buffers.
            self.buffers.append(self.compose.neural)
            self.key=key
        self.geometry=geometry
        hkey=(width,height,fmt,crop,live.profile,ih,iw)
        if hkey!=self.history_key:
            self.history_frames=0
            self.history_key=hkey
        self.compose.configure(intensity=live.intensity,history_confidence=live.temporal,
            history_hold=live.hold,history_release=live.release,
            blend_scale=getattr(self.backend,"blend_scale",nr_frame.BLEND_SCALE),
            grade=nr_frame.grade_for(**nr_frame.PROFILES[live.profile]),keep_neural=live.temporal>0)
        return ih,iw,ah,aw

    def process_device(self, raw, width, height, fmt, live, motion=None, output=None):
        """Return a device wire-image buffer. Only line maxima and scalar reductions are read.

        ``motion``: the device address of each pixel's offset to where its surface was on the
        frame before this pipeline was last given, two float32 a pixel at the frame's extent,
        NaN where it was not on screen; the history then follows it (`History.follow`).
        ``output``: a buffer of the frame's bytes for the composition to write the answer into,
        which is then what is returned - the transport's shared output, saving its copy. On a
        refused frame it may hold part of an answer; the transport hands it on only on success."""
        reason=self.supported(width,height,fmt,live)
        if reason: raise ValueError(reason)
        try:
            return self._process_device(raw,width,height,fmt,live,motion,output)
        except Exception:
            self.rt.abort()
            self.reset_history()
            raise

    def _follower(self, width, height, crop, ih, iw):
        if self.follower is None:
            self.follower=GPUImageFollower(self.rt,frame_size=(height,width),crop=crop,inner_size=(ih,iw))
        return self.follower

    def _process_device(self, raw, width, height, fmt, live, motion=None, output=None):
        started=time.perf_counter()
        crop=self.stats.region(raw,width,height,fmt)
        ih,iw,ah,aw=self._configure(width,height,fmt,crop,live)
        follow=motion is not None and live.temporal>0 and self.history_frames>0
        follower=self._follower(width,height,crop,ih,iw) if follow else None
        # One submit for the decode and what reads it before the CPU decides anything: the cut
        # test, or with motion the follower. They ran as separate submits in this order, and
        # each image pass is still recorded behind a barrier (`xmx_rec_image_pipeline`).
        self.rt.begin()
        self.decode.record(raw,self.active)
        self.inner_decode.record(raw,self.inner)
        cut_of=None
        if follow:
            self.history_resize.record(self.previous_neural,follower.resampled)
            follower.record(motion,neural=self.previous_neural,previous=self.previous_active,
                            active=self.active,previous_inner=self.previous_inner,inner=self.inner)
        elif self.history_frames:
            cut_of=self.stats.record_delta(self.inner,self.previous_inner,iw,ih,slot="cut")
        self.rt.submit()
        if follow:
            # Known nowhere, or a cut even against the last frame moved here: no history.
            cut=follower.cut(self.stats,self.inner,live.cut_limit)
            history=cut is not None and cut<=live.cut_limit
            cut=0.0 if cut is None else cut
        else:
            cut=cut_of(live.cut_limit) if cut_of is not None else 0.0
            history=live.temporal>0 and self.history_frames>0 and cut<=live.cut_limit
        if not history: self.history_frames=0
        self.rt.begin()
        if history and not follow: self.history_resize.record(self.previous_neural,self.history_inner)
        inner_history=(self.follower.inner if follow else self.history_inner) if history else None
        self.prep.record(raw,self.frame.input_buffer(),
            history=inner_history,**nr_frame.PROFILES[live.profile])
        self.rt.submit()
        prepared=time.perf_counter()
        head=self.frame.run_device()
        ran=time.perf_counter()
        # The head's check, the composition and the change it made, in one submit. The
        # composition now runs before the check is read; when the head is not finite its answer
        # is dropped as before (the transport copies it out only on success), the history
        # restarts, and the buffers do not trade places, so the neural it kept is never read.
        self.rt.begin()
        nonfinite_of=self.stats.record_nonfinite(head,self.geometry.network_width,
            self.geometry.network_height,4 if self.rt.compact_head else 16,slot="head")
        if history and follow:
            result=self.compose.record(raw,head,history=self.follower.full,previous=self.follower.pixels,
                                       target=output,composed=self.change_metric)
        else:
            result=self.compose.record(raw,head,history=self.previous_neural if history else None,
                                       previous=self.previous_active if history else None,
                                       target=output,composed=self.change_metric)
        change_of=(self.stats.record_delta(self.compose.composed,self.active,aw,ah,slot="change")
                   if self.change_metric else None)
        self.rt.submit()
        if nonfinite_of():
            self.history_frames=0
            raise ValueError("GPU network head contains nonfinite values; original frame retained")
        composed=time.perf_counter()
        change=change_of() if change_of is not None else None
        if live.temporal>0 and not HISTORY_SWAP:
            self.rt.begin()
            self.rt.copy(self.compose.neural,self.previous_neural,aw*ah*3*4)
            self.rt.copy(self.inner,self.previous_inner,iw*ih*3*4)
            self.rt.copy(self.active,self.previous_active,aw*ah*3*4)
            self.rt.submit()
        elif live.temporal>0:
            # This frame's buffers become the previous ones by trading places, not by copies:
            # three full-frame copies were 0.7 ms of a 1280x720 frame and 1.4 at 1920x1080.
            # Every submit above has been waited for, so nothing still reads either side, and
            # each buffer the next frame writes into is one whose contents no one needs: the
            # frame before this one's. Each target is handed to its pass at recording time.
            self.compose.neural,self.previous_neural=self.previous_neural,self.compose.neural
            self.inner,self.previous_inner=self.previous_inner,self.inner
            self.active,self.previous_active=self.previous_active,self.active
        self.history_frames=self.history_frames+1 if live.temporal>0 else 0
        self.last={"preprocess_ms":1000*(prepared-started),"network_ms":1000*(ran-prepared),
                   "compose_ms":1000*(composed-ran),"metadata_history_ms":1000*(time.perf_counter()-composed),
                   "network":(self.geometry.network_width,self.geometry.network_height),
                   "change":change,"cut":cut,"history":history,"motion":follow,"crop":crop}
        self.backend.split=(0.0,ran-prepared,0.0)
        return result

    def process_host(self,payload,width,height,fmt,live):
        reason=self.supported(width,height,fmt,live)
        if reason: raise ValueError(reason)
        nbytes=width*height*4
        if memoryview(payload).nbytes!=nbytes: raise ValueError("raw image byte count does not match geometry")
        if self.raw is None or self.raw.nbytes!=nbytes:
            if self.raw is not None: self.raw.free()
            self.raw=self.rt.buffer(nbytes,np.uint8,kind=X.DEVICE)
            self.wire=bytearray(nbytes)
        self.raw.upload(np.frombuffer(payload,np.uint8))
        result=self.process_device(self.raw,width,height,fmt,live)
        return result.download_into(self.wire)

    def close(self):
        self._clear()
        if self.raw is not None: self.raw.free();self.raw=None
        self.stats.close()
        self.wire=None
        self._closed=True
