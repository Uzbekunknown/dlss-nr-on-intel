"""Reduced GPU metadata versus Letterbox/cut/finite reference decisions.

--host-only exercises buffer validation and cache logic without Vulkan. Device
checks permit only the small HOST_READ reduction buffers to reach the host.
"""
from __future__ import annotations
import ctypes
import pathlib
import sys
import unittest
from unittest.mock import patch
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"src/ref"))
sys.path.insert(0,str(ROOT/"src/layer"))
import nr_daemon
import xmxres as X
import gpu_image_stats as G


class FakeBuffer:
    def __init__(self,runtime,count,dtype,kind):
        self.id=runtime.next_id;runtime.next_id+=1
        self._lib=runtime.lib
        self.address=self.id*4096
        self.nbytes=int(count)*np.dtype(dtype).itemsize
        self.kind=kind
        self.downloads=[]
        self.bytes=np.zeros(self.nbytes,np.uint8)
        runtime.buffers[self.address]=self

    def upload(self,values):
        raw=np.ascontiguousarray(values).view(np.uint8).reshape(-1)
        self.bytes[:raw.size]=raw
        return self

    def download(self,dtype=np.float32,count=None):
        if self.kind!=X.HOST_READ:
            raise AssertionError("the metadata implementation tried to read image pixels")
        self.downloads.append((np.dtype(dtype),count))
        return self.bytes.view(dtype)[:count].copy()

    def free(self): self.id=-1


class FakeRuntime:
    def __init__(self):
        self.lib=object();self.next_id=1;self.buffers={};self.recorded=0
        self.calls=[];self.aborts=0

    def buffer(self,count,dtype=np.float32,kind=X.DEVICE):
        return FakeBuffer(self,count,dtype,kind)

    def begin(self): self.recorded=0
    def submit(self): pass
    def abort(self): self.aborts+=1
    def copy(self,source,target,nbytes): target.bytes[:nbytes]=source.bytes[:nbytes]


class FakePipeline:
    def __init__(self,runtime): self.rt=runtime

    def record(self,push,groups):
        p=G._Push.from_buffer_copy(bytes(push))
        self.rt.calls.append((p.mode,groups))
        source=self.rt.buffers[p.a].bytes
        target=self.rt.buffers[p.out].bytes
        if p.mode in (4,5):
            count=(p.width*p.height*p.channels+1023)//1024
            partials=source.view(np.float32)[:count]
            if p.mode==4:
                values=np.array([partials.sum(dtype=np.uint64)],np.uint32)
            else:
                hi,lo=compensated_sum(partials)
                bits=partials.view(np.uint32)
                subnormal=((bits&0x7f800000)==0)&((bits&0x007fffff)!=0)
                status=(1 if np.isnan(partials).any() else 2 if np.isinf(partials).any()
                        else 3 if subnormal.any() or not np.isfinite((hi,lo)).all() else 0)
                values=np.array((hi,lo,0),np.float32).view(np.uint32)
                values[2]=status
        elif p.mode in (0,3):
            rgb=source.reshape(p.height,p.width,4)[...,:3]
            rows=rgb.max(axis=(1,2));columns=rgb.max(axis=(0,2))
            if p.mode==0:
                values=np.concatenate((rows,columns)).astype(np.uint32)
            else:
                top,bottom,left,right=p.bounds
                values=np.array((rows[top-1] if top else 0xffffffff,
                    rows[bottom] if bottom<p.height else 0xffffffff,
                    columns[left-1] if left else 0xffffffff,
                    columns[right] if right<p.width else 0xffffffff,
                    rows[top],rows[bottom-1],columns[left],columns[right-1]),np.uint32)
        else:
            a=source.view(np.float32)[:p.width*p.height*p.channels]
            if p.mode==1:
                b=self.rt.buffers[p.b].bytes.view(np.float32)[:a.size]
                items=np.abs(a-b)
            else: items=(~np.isfinite(a)).astype(np.float32)
            values=np.array([items[i:i+1024].sum(dtype=np.float32)
                             for i in range(0,a.size,1024)],np.float32).view(np.uint32)
        target[:values.nbytes]=values.view(np.uint8)


def compensated_sum(values):
    """CPU FP32 emulation of the shader's exact lane/reduction order."""
    def two(a,b):
        s=np.float32(a+b)
        from_b=np.float32(s-a)
        e=np.float32(np.float32(a-np.float32(s-from_b))+np.float32(b-from_b))
        return s,e
    def add(a,b):
        hi=two(a[0],b[0]);lo=two(a[1],b[1])
        combined=two(hi[0],np.float32(hi[1]+lo[0]))
        return two(combined[0],np.float32(combined[1]+lo[1]))
    pairs=np.zeros((256,2),np.float32)
    with np.errstate(over="ignore",invalid="ignore"):
        for i,v in enumerate(values):
            if np.isfinite(v): pairs[i%256]=add(pairs[i%256],(v,np.float32(0)))
        step=128
        while step:
            for lane in range(step): pairs[lane]=add(pairs[lane],pairs[lane+step])
            step//=2
    return pairs[0]


class HostChecks(unittest.TestCase):
    def setUp(self):
        self.rt=FakeRuntime()
        self.patch=patch.object(X,"ImagePipeline",lambda path:FakePipeline(self.rt))
        self.patch.start();self.addCleanup(self.patch.stop)
        self.stats=G.GPUImageStats(self.rt);self.addCleanup(self.stats.close)

    def test_push_abi_and_validation(self):
        self.assertEqual(ctypes.sizeof(G._Push),56)
        self.assertEqual(G._Push.bounds.offset,40)
        source=self.rt.buffer(10*12*4,np.uint8)
        for size in ((0,10),(1.0,10),(True,10),(0xffffffff,2)):
            with self.assertRaises(ValueError): self.stats.region(source,*size,37)
        with self.assertRaises(ValueError): self.stats.region(source,12,10,97)
        with self.assertRaises(ValueError): self.stats.region(source,12,11,37)
        source._lib=object()
        with self.assertRaises(ValueError): self.stats.region(source,12,10,37)
        source._lib=self.rt.lib
        source.free()
        with self.assertRaises(ValueError): self.stats.region(source,12,10,37)
        self.assertEqual(self.rt.calls,[])

    def test_cached_letterbox_matches_reference_through_changes(self):
        rng=np.random.default_rng(448)
        reference=nr_daemon.Letterbox()
        for height,width,fmt,top,bottom,left,right in (
                (41,73,37,4,37,0,73),(41,73,37,4,37,0,73),
                (41,73,37,0,41,0,73),(41,73,44,0,41,3,70),
                (42,74,43,2,39,0,74),(42,74,43,0,42,0,74),
                (42,74,50,3,38,5,69),(42,74,50,0,42,0,74)):
            raw=rng.integers(3,256,(height,width,4),dtype=np.uint8)
            raw[:top,:,:3]=2;raw[bottom:,:,:3]=0
            raw[:,:left,:3]=0;raw[:,right:,:3]=1
            colour=nr_daemon.decode(raw.tobytes(),width,height,fmt)
            expected=reference.region(colour,(width,height,fmt))
            source=self.rt.buffer(raw.size,np.uint8).upload(raw)
            try: self.assertEqual(self.stats.region(source,width,height,fmt),expected)
            finally: source.free()
        # a workgroup a line: all 41 + 73 of the first frame's, then the cached check's eight
        self.assertIn((3,8),self.rt.calls)
        self.assertEqual(self.rt.calls[0],(0,41+73))
        self.assertEqual(self.rt.calls[1],(3,8))

    def test_dark_alpha_and_asymmetric_lines(self):
        rng=np.random.default_rng(338)
        for height,width in ((1,1),(2,2),(21,37),(96,129)):
            for case in range(8):
                raw=rng.integers(0,256,(height,width,4),dtype=np.uint8)
                if case==0: raw[...,:3]=0
                if case==1: raw[...,:3]=2
                if case==2: raw[...,:3]=3
                if case>2:
                    margin=max(1,height//10)
                    raw[:margin,:,:3]=0
                    raw[-margin-(case%3):,:,:3]=0
                source=self.rt.buffer(raw.size,np.uint8).upload(raw)
                try:
                    expected=nr_daemon.active_region(nr_daemon.decode(raw.tobytes(),width,height,44))
                    # Match the cached CPU algorithm, including its own cache decision.
                    reference=nr_daemon.Letterbox()
                    reference.key=self.stats.key;reference.bounds=self.stats.bounds
                    expected=reference.region(nr_daemon.decode(raw.tobytes(),width,height,44),
                                              (width,height,44))
                    self.assertEqual(self.stats.region(source,width,height,44),expected)
                finally: source.free()

    def test_delta_and_nonfinite(self):
        rng=np.random.default_rng(239)
        a=rng.random((37,53,3),dtype=np.float32)
        b=rng.random(a.shape,dtype=np.float32)
        sa=self.rt.buffer(a.size).upload(a);sb=self.rt.buffer(b.size).upload(b)
        self.assertAlmostEqual(self.stats.delta(sa,sb,53,37),float(np.abs(a-b).mean()),places=7)
        self.assertEqual(self.rt.calls[-2:],[(1,6),(5,1)])
        self.assertEqual(self.stats.scalars.downloads[-1][1],3)
        self.assertFalse(self.stats.partials.downloads)
        self.assertEqual(self.stats.nonfinite(sa,53,37,3),0)
        self.assertEqual(self.rt.calls[-2:],[(2,6),(4,1)])
        self.assertEqual(self.stats.scalars.downloads[-1][1],1)
        a.reshape(-1)[[0,1023,1024,-1]]=[np.nan,np.inf,-np.inf,np.nan]
        sa.upload(a)
        self.assertEqual(self.stats.nonfinite(sa,53,37,3),4)
        with self.assertRaises(ValueError): self.stats.delta(sa,None,53,37)
        self.stats.close()
        with self.assertRaises(RuntimeError): self.stats.nonfinite(sa,53,37,3)

    def test_delta_matches_old_fp64_sum_and_threshold_edges_fall_back(self):
        rng=np.random.default_rng(2389)
        for shape in ((17,37,3),(97,129,3),(320,512,3)):
            a=rng.random(shape,dtype=np.float32)
            b=a*np.float32(.65)
            sa=self.rt.buffer(a.size).upload(a);sb=self.rt.buffer(b.size).upload(b)
            got=self.stats.delta(sa,sb,shape[1],shape[0])
            count=(a.size+1023)//1024
            old=float(self.stats.partials.bytes.view(np.float32)[:count].sum(dtype=np.float64)/a.size)
            bound=self.stats.last_delta_error_bound
            self.assertLessEqual(abs(got-old),bound)
            self.assertLess(bound,1e-8)
            self.assertFalse(self.stats.last_delta_fallback)
            # Even a threshold equal to the previous FP64 answer must preserve
            # its exact <= decision, rather than relying on approximate last bits.
            guarded=self.stats.delta(sa,sb,shape[1],shape[0],decision_threshold=old)
            self.assertEqual(guarded,old)
            self.assertTrue(self.stats.last_delta_fallback)
            self.assertEqual(self.stats.last_delta_error_bound,0.)
            self.stats.delta(sa,sb,shape[1],shape[0],decision_threshold=.5)
            self.assertFalse(self.stats.last_delta_fallback)
            sa.free();sb.free()

    def test_compensated_normal_range_vs_fp64_reference(self):
        rng=np.random.default_rng(2592)
        u32=float(np.finfo(np.float32).eps)/2
        u64=float(np.finfo(np.float64).eps)/2
        for count in (1,7,255,256,257,4097,12288):
            for kind in range(4):
                values=rng.uniform(0,1024,count).astype(np.float32)
                if kind==1: values*=np.float32(1e-7)
                if kind==2:
                    values=np.exp2(rng.uniform(-40,10,count)).astype(np.float32)
                if kind==3:
                    values[1::2]=np.float32(2**-24)
                hi,lo=compensated_sum(values)
                elements=count*1024
                got=float((np.float64(hi)+np.float64(lo))/elements)
                old=float(values.sum(dtype=np.float64)/elements)
                relative=16*count*u32*u32+4*count*u64
                bound=relative/(1-relative)*abs(got)+16*count*float(np.finfo(np.float32).tiny)/elements
                self.assertLessEqual(abs(got-old),bound)
                self.assertLess(bound,1e-8)

    def test_final_nonfinite_count_above_fp32_integer_precision(self):
        # 2²⁴+1 cannot be kept in one FP32 value. Each first-pass count is
        # exactly representable and the uint final reduction must retain the +1.
        counts=np.full(16385,1024,np.float32);counts[-1]=1
        source=self.rt.buffer(counts.size).upload(counts)
        target=self.rt.buffer(3,np.uint32,kind=X.HOST_READ)
        push=G._Push(source.address,0,target.address,16777217,1,4,1,(ctypes.c_uint32*4)())
        self.stats.pipeline.record(push,1)
        self.assertEqual(int(target.download(np.uint32,1)[0]),16777217)

    def test_delta_nan_inf_and_finite_overflow_status(self):
        a=self.rt.buffer(2048).upload(np.ones(2048,np.float32))
        b=self.rt.buffer(2048).upload(np.zeros(2048,np.float32))
        self.assertTrue(np.isfinite(self.stats.delta(a,b,2048,1,1)))
        for special in (np.nan,np.inf):
            values=np.ones(2048,np.float32);values[0]=special;a.upload(values)
            got=self.stats.delta(a,b,2048,1,1)
            self.assertTrue(np.isnan(got) if np.isnan(special) else np.isinf(got))
        # Partials remain finite, but their FP32 pair can overflow. This
        # uncommon non-image range uses the unchanged FP64 metadata sum.
        a.upload(np.full(2048,np.float32(2e35)))
        got=self.stats.delta(a,b,2048,1,1)
        old=float(self.stats.partials.bytes.view(np.float32)[:2].sum(dtype=np.float64)/2048)
        self.assertTrue(np.isfinite(got))
        self.assertEqual(got,old)
        self.assertTrue(self.stats.last_delta_fallback)
        a.upload(np.zeros(2048,np.float32))
        self.assertEqual(self.stats.delta(a,b,2048,1,1,decision_threshold=0.),0.)
        self.assertTrue(self.stats.last_delta_fallback)
        a.upload(np.full(2048,np.finfo(np.float32).smallest_subnormal,np.float32))
        got=self.stats.delta(a,b,2048,1,1,decision_threshold=0.)
        old=float(self.stats.partials.bytes.view(np.float32)[:2].sum(dtype=np.float64)/2048)
        self.assertEqual(got,old)
        self.assertTrue(self.stats.last_delta_fallback)
        self.assertGreater(got,0.)

    def test_failed_recording_aborts(self):
        source=self.rt.buffer(8*8*4,np.uint8)
        with patch.object(self.stats.pipeline,"record",side_effect=RuntimeError("dispatch failed")):
            with self.assertRaisesRegex(RuntimeError,"dispatch failed"):
                self.stats.region(source,8,8,37)
        self.assertEqual(self.rt.aborts,1)


class DeviceChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.rt=X.Runtime()

    def setUp(self):
        self.stats=G.GPUImageStats(self.rt);self.addCleanup(self.stats.close)

    def test_device_scalar_delta_vs_old_fp64_and_threshold_decisions(self):
        rng=np.random.default_rng(2884)
        original=X.Buffer.download
        downloads=[]
        def reduced_only(buffer,*args,**kwargs):
            if buffer.kind!=X.HOST_READ:
                raise AssertionError("DEVICE download while reducing metadata")
            count=kwargs.get("count",args[1] if len(args)>1 else None)
            downloads.append(count)
            return original(buffer,*args,**kwargs)
        with patch.object(X.Buffer,"view",side_effect=AssertionError("host image view")),\
             patch.object(X.Buffer,"download",reduced_only):
            for shape in ((17,37,3),(320,512,3),(576,1024,3)):
                a=rng.random(shape,dtype=np.float32)
                b=a*np.float32(.65)
                sa=self.rt.buffer(a.size,kind=X.DEVICE).upload(a)
                sb=self.rt.buffer(b.size,kind=X.DEVICE).upload(b)
                try:
                    got=self.stats.delta(sa,sb,shape[1],shape[0])
                    self.assertEqual(downloads[-1],3)
                    self.assertFalse(self.stats.last_delta_fallback)
                    bound=self.stats.last_delta_error_bound
                    old=self.stats._exact_delta(a.size,(a.size+1023)//1024)
                    self.assertLessEqual(abs(got-old),bound)
                    self.assertLess(bound,1e-8)
                    for threshold in (old,np.nextafter(old,-np.inf),np.nextafter(old,np.inf)):
                        guarded=self.stats.delta(sa,sb,shape[1],shape[0],decision_threshold=threshold)
                        self.assertEqual(guarded,old)
                        self.assertEqual(guarded<=threshold,old<=threshold)
                        self.assertTrue(self.stats.last_delta_fallback)
                    self.stats.delta(sa,sb,shape[1],shape[0],decision_threshold=.5)
                    self.assertFalse(self.stats.last_delta_fallback)
                    self.assertEqual(downloads[-1],3)
                finally: sa.free();sb.free()

    def test_device_final_count_and_compensated_pairs(self):
        counts=np.full(16385,1024,np.float32);counts[-1]=1
        source=self.rt.buffer(counts.size,kind=X.DEVICE).upload(counts)
        target=self.rt.buffer(3,np.uint32,kind=X.HOST_READ)
        try:
            self.rt.begin()
            self.stats.pipeline.record(G._Push(source.address,0,target.address,16777217,1,4,1,
                                               (ctypes.c_uint32*4)()),1)
            self.rt.submit()
            self.assertEqual(int(target.download(np.uint32,1)[0]),16777217)
            rng=np.random.default_rng(2987)
            for count in (1,257,4097,12288):
                values=np.exp2(rng.uniform(-40,10,count)).astype(np.float32)
                source.upload(np.pad(values,(0,source.nbytes//4-count)))
                self.rt.begin()
                self.stats.pipeline.record(G._Push(source.address,0,target.address,count*1024,1,5,1,
                                                   (ctypes.c_uint32*4)()),1)
                self.rt.submit()
                result=target.download(np.uint32,3)
                self.assertEqual(result[2],0)
                hi,lo=result[:2].view(np.float32)
                got=float((np.float64(hi)+np.float64(lo))/(count*1024))
                old=float(values.sum(dtype=np.float64)/(count*1024))
                self.assertLess(abs(got-old),1e-8)
            # A raw positive subnormal must request exact metadata fallback;
            # it must not silently become zero through FP32 FTZ in this pass.
            tiny=np.array([1],np.uint32).view(np.float32)
            source.upload(np.pad(tiny,(0,source.nbytes//4-1)))
            self.rt.begin()
            self.stats.pipeline.record(G._Push(source.address,0,target.address,1024,1,5,1,
                                               (ctypes.c_uint32*4)()),1)
            self.rt.submit()
            self.assertEqual(target.download(np.uint32,3)[2],3)
        finally: source.free();target.free()

    def test_device_regions_and_reductions_only_read_metadata(self):
        original=X.Buffer.download
        def reduced_only(buffer,*args,**kwargs):
            if buffer.kind!=X.HOST_READ:
                raise AssertionError("full image readback inside GPUImageStats")
            return original(buffer,*args,**kwargs)
        rng=np.random.default_rng(226)
        reference=nr_daemon.Letterbox()
        with patch.object(X.Buffer,"view",side_effect=AssertionError("host image view")),\
             patch.object(X.Buffer,"download",reduced_only):
            for height,width,fmt,top,bottom,left,right in (
                    (41,73,37,4,37,0,73),(41,73,37,4,37,0,73),
                    (41,73,37,0,41,0,73),(41,73,44,0,41,3,70),
                    (42,74,43,2,39,0,74),(42,74,43,0,42,0,74),
                    (42,74,50,3,38,5,69),(42,74,50,0,42,0,74),
                    (1,1,44,0,1,0,1),(192,320,37,0,192,0,320)):
                raw=rng.integers(3,256,(height,width,4),dtype=np.uint8)
                raw[:top,:,:3]=2;raw[bottom:,:,:3]=0
                raw[:,:left,:3]=0;raw[:,right:,:3]=1
                source=self.rt.buffer(raw.size,np.uint8,kind=X.DEVICE).upload(raw)
                try:
                    expected=reference.region(nr_daemon.decode(raw.tobytes(),width,height,fmt),
                                              (width,height,fmt))
                    self.assertEqual(self.stats.region(source,width,height,fmt),expected)
                finally: source.free()
            a=rng.normal(size=(33,61,4)).astype(np.float32)
            b=rng.normal(size=a.shape).astype(np.float32)
            sa=self.rt.buffer(a.size,kind=X.DEVICE).upload(a)
            sb=self.rt.buffer(b.size,kind=X.DEVICE).upload(b)
            try:
                self.assertAlmostEqual(self.stats.delta(sa,sb,61,33,4),float(np.abs(a-b).mean()),places=6)
                self.assertEqual(self.stats.nonfinite(sa,61,33,4),0)
                a.reshape(-1)[[0,1023,1024,-1]]=[np.nan,np.inf,-np.inf,np.nan]
                sa.upload(a)
                self.assertEqual(self.stats.nonfinite(sa,61,33,4),4)
            finally: sa.free();sb.free()


if __name__=="__main__":
    host_only="--host-only" in sys.argv
    sys.argv=[value for value in sys.argv if value!="--host-only"]
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(HostChecks)
    if not host_only: suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(DeviceChecks))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
