"""Read reduced metadata, never image arrays, for the GPU pixel path."""
from __future__ import annotations
import ctypes
import os
import pathlib
import numpy as np
import xmxres as X

ROOT=pathlib.Path(__file__).resolve().parents[2]

class _Push(ctypes.Structure):
    _fields_=[("a",ctypes.c_uint64),("b",ctypes.c_uint64),("out",ctypes.c_uint64),
              ("width",ctypes.c_uint32),("height",ctypes.c_uint32),
              ("mode",ctypes.c_uint32),("channels",ctypes.c_uint32),
              ("bounds",ctypes.c_uint32*4)]

assert ctypes.sizeof(_Push)==56
SDR_FORMATS=frozenset((37,43,44,50))

class GPUImageStats:
    def __init__(self,runtime):
        self.rt=runtime
        self.pipeline=X.ImagePipeline(os.environ.get("NR_IMAGE_STATS_SPV") or ROOT/"work/image_stats.spv")
        self.output=None
        self.partials=None
        self.scalars=None
        self.key=None
        self.bounds=None
        self._closed=False
        self.last_delta_error_bound=0.0
        self.last_delta_fallback=False

    def _storage(self,count):
        if self.output is None or self.output.nbytes<count*4:
            if self.output is not None: self.output.free()
            self.output=self.rt.buffer(count,np.uint32,kind=X.HOST_READ)
        return self.output

    def _reduction_storage(self,count):
        if self.partials is None or self.partials.nbytes<count*4:
            if self.partials is not None: self.partials.free()
            self.partials=self.rt.buffer(count,np.uint32,kind=X.DEVICE)
        if self.scalars is None:
            self.scalars=self.rt.buffer(3,np.uint32,kind=X.HOST_READ)
        return self.partials,self.scalars

    def _exact_delta(self,elements,count):
        """Old FP64 sum, reading only reduced metadata through HOST_READ scratch."""
        metadata=self._storage(count)
        self.rt.begin()
        try:
            self.rt.copy(self.partials,metadata,count*4)
            self.rt.submit()
        except Exception:
            self.rt.abort()
            raise
        return float(metadata.download(np.float32,count).sum(dtype=np.float64)/elements)

    def _run(self,a,b,width,height,mode,channels=3,bounds=None):
        if self._closed:
            raise RuntimeError("GPUImageStats is closed")
        if any(not isinstance(v,(int,np.integer)) or isinstance(v,(bool,np.bool_))
               or v<1 for v in (width,height,channels)):
            raise ValueError("invalid metadata extent")
        width,height,channels=int(width),int(height),int(channels)
        elements=width*height*channels
        if elements>0xffffffff or width+height>0xffffffff:
            raise ValueError("invalid metadata extent")
        if mode not in (0,1,2,3):
            raise ValueError("invalid metadata mode")
        if mode==3 and (bounds is None or len(bounds)!=4 or
                not all(isinstance(v,(int,np.integer)) for v in bounds) or
                not (0<=bounds[0]<bounds[1]<=height and 0<=bounds[2]<bounds[3]<=width)):
            raise ValueError("cached metadata bounds must lie inside the image")
        required=width*height*4 if mode in (0,3) else elements*4
        for name,buffer in (("source",a),("previous",b)):
            if buffer is None and name=="previous" and mode!=1:
                continue
            if (buffer is None or getattr(buffer,"id",-1)<0 or
                    getattr(buffer,"nbytes",0)<required):
                raise ValueError(f"{name} must be a live buffer of at least {required} bytes")
            if getattr(buffer,"_lib",self.rt.lib) is not self.rt.lib:
                raise ValueError(f"{name} belongs to another Vulkan runtime")
        count=width+height if mode==0 else 8 if mode==3 else (elements+1023)//1024
        out=self._storage(count) if mode in (0,3) else self._reduction_storage(count)[0]
        if any(buffer is not None and buffer.id==out.id for buffer in (a,b)):
            raise ValueError("metadata output must not alias its inputs")
        push=_Push(a.address,b.address if b is not None else 0,out.address,
                   width,height,mode,channels,(ctypes.c_uint32*4)(*(bounds or (0,0,0,0))))
        self.rt.begin()
        try:
            # One workgroup per output word: a line in modes 0 and 3 (image_stats.comp), a
            # partial sum in modes 1 and 2.
            self.pipeline.record(push,count)
            self.rt.recorded+=1
            if mode in (1,2):
                final=_Push(out.address,0,self.scalars.address,width,height,
                            5 if mode==1 else 4,channels,(ctypes.c_uint32*4)(0,0,0,0))
                self.pipeline.record(final,1)
                self.rt.recorded+=1
            self.rt.submit()
        except Exception:
            self.rt.abort()
            raise
        if mode in (1,2):
            return self.scalars.download(np.uint32,3 if mode==1 else 1)
        return out.download(np.uint32,count)

    def delta(self,a,b,width,height,channels=3,*,decision_threshold=None):
        """Mean delta from three scalar words: FP32 high/low sum and status.

        Compensation preserves substantially more precision than an FP32 final
        sum, but does not promise the old FP64 host sum's last bits. A caller
        making a cut decision can supply its threshold; overlapping error bounds
        fall back to the unchanged FP64 reduction of first-pass metadata. No
        image is downloaded, including on this uncommon boundary path.
        """
        if decision_threshold is not None and not np.isfinite(decision_threshold):
            raise ValueError("delta decision threshold must be finite")
        words=self._run(a,b,width,height,1,channels)
        elements=int(width)*int(height)*int(channels)
        count=(elements+1023)//1024
        status=int(words[2])
        self.last_delta_fallback=False
        if status==1:
            self.last_delta_error_bound=0.0
            return float("nan")
        if status==2:
            self.last_delta_error_bound=0.0
            return float("inf")
        hi,lo=words[:2].view(np.float32)
        value=float((np.float64(hi)+np.float64(lo))/elements)
        # Conservative positive-sum bound for normalized two-component additions:
        # 16*n*u32² covers accumulated compensation rounding;4*n*u64 also
        # covers the old host FP64 reduction's rounding. For daemon image extents
        # <=2²² pixels,3 channels and mean<=1 this is below7.1e-10. An absolute
        # tiny term covers possible FP32 subnormal flushing in the compensation;
        # finite overflow is handled by exact metadata fallback below.
        u32=float(np.finfo(np.float32).eps)/2
        u64=float(np.finfo(np.float64).eps)/2
        relative=16*count*u32*u32+4*count*u64
        tiny=16*count*float(np.finfo(np.float32).tiny)/elements
        bound=(relative/(1-relative))*abs(value)+tiny if relative<1 else float("inf")
        self.last_delta_error_bound=bound
        uncertain=(decision_threshold is not None and abs(value-float(decision_threshold))<=bound)
        if status==3 or uncertain:
            value=self._exact_delta(elements,count)
            self.last_delta_error_bound=0.0
            self.last_delta_fallback=True
        return value

    def nonfinite(self,a,width,height,channels=4):
        return int(self._run(a,None,width,height,2,channels)[0])

    def region(self,raw,width,height,vk_format):
        if vk_format not in SDR_FORMATS:
            raise ValueError("GPU letterbox metadata supports SDR RGBA8/BGRA8 only")
        key=(width,height,vk_format)
        if self.key==key and self.bounds is not None:
            checks=self._run(raw,None,width,height,3,bounds=self.bounds)
            if all(value==0xffffffff or bool(value<=2)==(i<4)
                   for i,value in enumerate(checks)):
                return self.bounds
        maxima=self._run(raw,None,width,height,0)
        rows,columns=maxima[:height],maxima[height:]
        def run(values,extent):
            cap=int(extent*.45);lead=tail=0
            while lead<cap and values[lead]<=2: lead+=1
            while tail<cap and values[extent-1-tail]<=2: tail+=1
            return (lead,extent-tail) if lead and tail and abs(lead-tail)<=1 else (0,extent)
        top,bottom=run(rows,height);left,right=run(columns,width)
        if (bottom-top)*(right-left)<width*height//2:
            top,bottom,left,right=0,height,0,width
        self.key,self.bounds=key,(top,bottom,left,right)
        return self.bounds

    def close(self):
        if self.output is not None: self.output.free();self.output=None
        if self.partials is not None: self.partials.free();self.partials=None
        if self.scalars is not None: self.scalars.free();self.scalars=None
        self.key=self.bounds=None
        self._closed=True
