#!/usr/bin/env python3
"""Compare CPU images, GPU images with host transport, and resident GPU images.

All three modes run the full model on the same ordered SDR frames/settings.
The two daemon modes call the real process_connection using an in-memory socket
fixture; the device mode excludes fixture upload/final readback from its timer.
These are daemon/image-processing measurements, not game FPS. No game, registry,
global layer or installation is modified.

    python src/bench/gpu_frame_stages.py --output work/gpu-frame-stages.json
    python src/bench/gpu_frame_stages.py --describe        # no Vulkan device
    python src/bench/gpu_frame_stages.py --smoke           # small real model

On Windows use the checkout's native Python and NR_DLL_PATH for libgomp and
libwinpthread. Run serially with every other GPU test/application benchmark.
"""
from __future__ import annotations
import argparse
import contextlib
import datetime
import hashlib
import io
import json
import os
import pathlib
import platform
import struct
import subprocess
import sys
import time
from unittest.mock import patch

import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[2]
os.environ.setdefault("NR_ROOT",str(ROOT))
for directory in ("gpu","ref","layer"):
    sys.path.insert(0,str(ROOT/"src"/directory))
import nr_alloc
import nr_daemon as D
import nr_frame
import nr_image
import xmx
import xmxres as X
from gpu_image_pipeline import GPUImagePipeline

MODES=("cpu-daemon","gpu-image-host-daemon","gpu-device")


class QuietLog(io.TextIOBase):
    """Keep normal daemon formatting/flush calls but avoid terminal I/O variance."""
    def __init__(self): self.last=""
    def write(self,text):
        if text.strip(): self.last=text.rstrip()
        return len(text)
    def flush(self): pass


class MemoryConnection:
    """The daemon's socket interface without kernel IPC or an extra reply copy."""
    def __init__(self,payload,width,height,fmt):
        self.header=memoryview(struct.pack("<4I",D.MAGIC,width,height,fmt))
        self.payload=memoryview(payload).cast("B")
        self.header_at=self.payload_at=0
        self.reply=None
        self.body_finished=self.answer_ready=None
    def recv_into(self,target,count):
        target=memoryview(target).cast("B")
        if self.header_at<len(self.header):
            got=min(count,len(self.header)-self.header_at)
            target[:got]=self.header[self.header_at:self.header_at+got]
            self.header_at+=got
            return got
        got=min(count,len(self.payload)-self.payload_at)
        target[:got]=self.payload[self.payload_at:self.payload_at+got]
        self.payload_at+=got
        if self.payload_at==len(self.payload): self.body_finished=time.perf_counter()
        return got
    def sendall(self,answer):
        self.reply=memoryview(answer).cast("B")
        self.answer_ready=time.perf_counter()


class TracedBackend(nr_frame.ResidentBackend):
    """Mark CPU preparation boundaries without instrumenting individual GPU passes."""
    def run_features(self,features):
        if getattr(self,"timestamp_network",False): X.profile_reset()
        self.network_started=time.perf_counter()
        try:
            result=super().run_features(features)
            if getattr(self,"timestamp_network",False): self.last_network_profile=network_profile()
            return result
        finally: self.network_finished=time.perf_counter()


def network_profile():
    totals=X.profile_totals()
    return dict(device_ms=sum(value[0] for value in totals.values()),
                passes=sum(value[1] for value in totals.values()),
                families={f"{family}/{kind}":{"ms":value[0],"passes":value[1]}
                          for (family,kind),value in totals.items()})


def case_spec(value):
    try:
        extent,scale=value.split("@")
        width,height=map(int,extent.lower().split("x"))
        scale=float(scale)
    except (ValueError,TypeError):
        raise argparse.ArgumentTypeError("case must be WIDTHxHEIGHT@SCALE") from None
    if width<1 or height<1 or not .05<=scale<=1:
        raise argparse.ArgumentTypeError("case extent/scale is invalid")
    return width,height,scale


def fixture(width,height):
    """Four gentle deterministic changes: gradients/edges/chroma with stable alpha.

    A generated fixture is reproducible and does not require shipping a game
    capture. It exercises temporal history, crop checks and full-frame passes;
    it does not represent the quality/content of a particular game.
    """
    y,x=np.indices((height,width),dtype=np.uint32)
    base=np.empty((height,width,4),np.uint8)
    base[...,0]=(13+(x//7+y//3)%227).astype(np.uint8)
    base[...,1]=(19+(x//4+2*y//9)%221).astype(np.uint8)
    base[...,2]=(23+(3*x//11+y//5)%217).astype(np.uint8)
    base[...,3]=((x+3*y)%256).astype(np.uint8)
    frames=[]
    for turn in range(4):
        frame=base.copy()
        # Keep the sequence well below the shot-cut threshold, including 3->0.
        frame[...,0]+=np.uint8(turn)
        frame[height//3:height//2,width//3:width//2,1]+=np.uint8(turn)
        frames.append(frame.tobytes())
    return frames


def live_args(width,height,scale,minimum,profile):
    args=argparse.Namespace(settings=None,profile=profile,intensity=1.,detail_strength=1.,
        colour_strength=1.,render_scale=scale,temporal=1.,hold=1.,release=24.,
        cut_limit=.15,min_extent=minimum,dump=None,meter=None)
    args.live=D.Settings(args)
    args.history=D.History()
    args.letterbox=D.Letterbox()
    return args


def quantiles(rows):
    fields=set().union(*(row.keys() for row in rows))
    return {name:{"median":float(np.median([row[name] for row in rows])),
                  "p95":float(np.percentile([row[name] for row in rows],95)),
                  "min":float(min(row[name] for row in rows)),
                  "max":float(max(row[name] for row in rows))}
            for name in sorted(fields)}


def daemon_step(mode,backend,args,payload,width,height,fmt):
    image_mode=mode!="cpu-daemon"
    connection=MemoryConnection(payload,width,height,fmt)
    started=time.perf_counter()
    D.process_connection(connection,backend,args)
    finished=time.perf_counter()
    if connection.reply is None or len(connection.reply)!=width*height*4:
        raise RuntimeError(f"{mode} did not return the expected SDR frame")
    expected_transport="gpu-image-host" if image_mode else "cpu"
    if getattr(args,"image_transport",None)!=expected_transport:
        raise RuntimeError(f"{mode} silently fell back to {args.image_transport!r}")
    row=dict(daemon_total_ms=1000*(finished-started),
        receive_fixture_ms=1000*(connection.body_finished-started),
        answer_ready_ms=1000*(connection.answer_ready-started),
        after_answer_ms=1000*(finished-connection.answer_ready))
    if image_mode:
        metrics=args.gpu_images.last
        for name in ("preprocess_ms","network_ms","compose_ms","metadata_history_ms"):
            row[name]=metrics[name]
        compute=1000*(connection.answer_ready-connection.body_finished)
        row["host_transfer_and_dispatch_ms"]=compute-sum(metrics[name] for name in
            ("preprocess_ms","network_ms","compose_ms","metadata_history_ms"))
    else:
        row["preprocess_ms"]=1000*(backend.network_started-connection.body_finished)
        row["network_ms"]=1000*(backend.network_finished-backend.network_started)
        row["compose_ms"]=1000*(connection.answer_ready-backend.network_finished)
        row["metadata_history_ms"]=row["after_answer_ms"]
        row.update(zip(("network_input_ms","network_graph_ms","network_read_ms"),
                       (1000*value for value in backend.split)))
    return row,hashlib.sha256(connection.reply).hexdigest()


def run_daemon_mode(mode,backend,args,frames,width,height,fmt,warm,samples,keeping):
    rows,hashes=[],[]
    log=QuietLog()
    image_mode=mode!="cpu-daemon"
    with patch.dict(os.environ,{"NR_GPU_IMAGE":"1" if image_mode else "0",
                                "NR_GPU_TRANSPORT":"0"}),contextlib.redirect_stdout(log):
        for turn in range(warm+samples):
            row,digest=daemon_step(mode,backend,args,frames[turn%len(frames)],width,height,fmt)
            if turn>=warm:
                rows.append(row)
                hashes.append(digest)
            if keeping: keeping.frame_done()
    if image_mode and not args.gpu_images.last["history"]:
        raise RuntimeError("temporal history was not engaged during the GPU daemon benchmark")
    return dict(samples=quantiles(rows),raw_samples_ms=rows,output_sha256=hashes,
                last_log=log.last,history_frames=(args.gpu_images.history_frames if image_mode
                                               else args.history.frames))


def device_step(pipe,raw,payload,width,height,fmt,live):
    raw.upload(np.frombuffer(payload,np.uint8))
    started=time.perf_counter()
    answer=pipe.process_device(raw,width,height,fmt,live)
    finished=time.perf_counter()
    row=dict(device_total_ms=1000*(finished-started))
    for name in ("preprocess_ms","network_ms","compose_ms","metadata_history_ms"):
        row[name]=pipe.last[name]
    return row,hashlib.sha256(answer.download(np.uint8)).hexdigest()


def run_device_mode(backend,args,frames,width,height,fmt,warm,samples,keeping):
    pipe=GPUImagePipeline(backend)
    raw=backend.runtime.buffer(width*height*4,np.uint8,kind=X.DEVICE)
    rows,hashes=[],[]
    try:
        for turn in range(warm+samples):
            row,digest=device_step(pipe,raw,frames[turn%len(frames)],width,height,fmt,args.live)
            if turn>=warm:
                rows.append(row)
                hashes.append(digest)
            if keeping: keeping.frame_done()
        if not pipe.last["history"]:
            raise RuntimeError("temporal history was not engaged during the device benchmark")
        return dict(samples=quantiles(rows),raw_samples_ms=rows,output_sha256=hashes,
                    history_frames=pipe.history_frames,last_metrics=pipe.last)
    finally:
        raw.free();pipe.close()


def allocation_notes(runtime):
    """Record the runtime's existing memory note after one allocation per kind.

    This identifies the chosen property/type policy; it does not measure whether
    that type causes a performance difference. The tiny buffers are freed before
    the benchmark, and mapped is queried without accessing image pixels.
    """
    out={}
    for name,kind in (("graph",X.GRAPH),("host-read",X.HOST_READ),
                      ("host-write",X.HOST_WRITE),("device",X.DEVICE)):
        buffer=runtime.buffer(16,kind=kind)
        try: out[name]=dict(kind=kind,mapped=buffer.mapped,note=xmx.memory_note())
        finally: buffer.free()
    return out


def run_interleaved(case,frames,cli,keeping,report):
    """Retain all mode contexts and rotate their order for every fixture frame."""
    width,height,scale=case["width"],case["height"],case["render_scale"]
    backend=TracedBackend(max_cached_frames=2)
    args={mode:live_args(width,height,scale,cli.minimum,cli.profile) for mode in MODES}
    pipe=GPUImagePipeline(backend)
    raw=backend.runtime.buffer(width*height*4,np.uint8,kind=X.DEVICE)
    rows={mode:[] for mode in MODES};hashes={mode:[] for mode in MODES}
    logs={mode:QuietLog() for mode in MODES}
    report["metadata"].setdefault("device",xmx.device_name())
    report["metadata"].setdefault("graph_key",backend.runtime.graph_key())
    report["metadata"].setdefault("allocation_notes",allocation_notes(backend.runtime))
    def step(mode,payload):
        if mode=="gpu-device":
            return device_step(pipe,raw,payload,width,height,cli.format,args[mode].live)
        with patch.dict(os.environ,{"NR_GPU_IMAGE":"1" if mode!="cpu-daemon" else "0",
                                    "NR_GPU_TRANSPORT":"0"}),contextlib.redirect_stdout(logs[mode]):
            return daemon_step(mode,backend,args[mode],payload,width,height,cli.format)
    try:
        for turn in range(cli.warm+cli.samples):
            payload=frames[turn%len(frames)]
            first=turn%len(MODES)
            for mode in MODES[first:]+MODES[:first]:
                row,digest=step(mode,payload)
                if turn>=cli.warm: rows[mode].append(row);hashes[mode].append(digest)
                if keeping: keeping.frame_done()
        for mode in MODES:
            if mode=="gpu-device": count=pipe.history_frames
            elif mode=="cpu-daemon": count=args[mode].history.frames
            else: count=args[mode].gpu_images.history_frames
            if count!=cli.warm+cli.samples:
                raise RuntimeError(f"{mode} history rebuilt unexpectedly during interleaving: {count}")
            case["modes"][mode]=dict(samples=quantiles(rows[mode]),raw_samples_ms=rows[mode],
                output_sha256=hashes[mode],history_frames=count,last_log=logs[mode].last)
        case["scheduler"]="retained contexts; per-frame order rotates CPU/host/device, host/device/CPU, device/CPU/host"
        if cli.profile_network:
            case["network_timestamp_probe"]=timestamp_probe(step,backend,frames,cli,keeping)
    finally:
        raw.free();pipe.close()
        for state in args.values(): D.close_gpu_images(state)
        backend.close()


def timestamp_probe(step,backend,frames,cli,keeping):
    """Separate instrumentation run; never mixed into uninstrumented totals.

    Single-recording execution writes timestamp queries for each network pass;
    an existing replay captured without queries cannot provide those counters.
    The graph/device conditions are identical for every mode within this probe.
    """
    import nr_frame_resident as resident
    rows={mode:[] for mode in MODES};details={mode:[] for mode in MODES}
    real_device=resident.ResidentFrame.run_device
    def device_network(frame,*args,**kwargs):
        X.profile_reset()
        answer=real_device(frame,*args,**kwargs)
        backend.last_network_profile=network_profile()
        return answer
    try:
        X.profile(True)
    except RuntimeError as error:
        return dict(available=False,reason=str(error))
    backend.timestamp_network=True
    try:
        with patch.dict(os.environ,{"NR_FRAME_MODE":"single"}),\
             patch.object(resident.ResidentFrame,"run_device",device_network):
            for turn in range(cli.warm+cli.profile_samples):
                payload=frames[(cli.warm+cli.samples+turn)%len(frames)]
                first=turn%len(MODES)
                for mode in MODES[first:]+MODES[:first]:
                    wall,_=step(mode,payload)
                    profile=backend.last_network_profile
                    if not profile["passes"]:
                        raise RuntimeError("GPU timestamp profiling produced no network passes")
                    if turn>=cli.warm:
                        rows[mode].append(dict(network_device_ms=profile["device_ms"],
                                               network_wall_ms=wall["network_ms"]))
                        details[mode].append(profile)
                    if keeping: keeping.frame_done()
        return dict(available=True,execution="single, timestamped separately from primary replay measurements",
            warmup_frames=cli.warm,measured_frames=cli.profile_samples,
            modes={mode:dict(samples=quantiles(rows[mode]),raw_samples_ms=rows[mode],
                             per_pass_family=details[mode]) for mode in MODES})
    finally:
        backend.timestamp_network=False;X.profile(False)


def metadata():
    relevant=("NR_ROOT","NR_DLL_PATH","NR_FRAME_MODE","NR_INPUT_VIEW","NR_INPUT_FP16",
        "NR_COMPACT_HEAD","NR_KEEP_BLOCKS","NR_HOST_NATIVE","NR_GPU_IMAGE","NR_GPU_TRANSPORT",
        "NR_SCRATCH_ARENA","XMX_DEVICE","XMX_QUEUE_PRIORITY","XMX_STAGING")
    selected={name:os.environ[name] for name in relevant if name in os.environ}
    # Explicit performance options only; no environment dump or unrelated data.
    out=dict(python=sys.version,executable=sys.executable,numpy=np.__version__,
        platform=platform.platform(),created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        configuration=selected,checkout=str(ROOT))
    if os.name=="nt":
        command="Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion | ConvertTo-Json -Compress"
        try:
            result=subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command",command],
                capture_output=True,text=True,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW,check=True)
            out["windows_video_drivers"]=json.loads(result.stdout)
        except (OSError,subprocess.SubprocessError,ValueError) as error:
            out["windows_video_driver_query_error"]=str(error)
    out["artifact_sha256"]={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in
        (ROOT/"work"/("libxmx.dll" if os.name=="nt" else "libxmx.so"),
         ROOT/"work/image_preprocess.spv",ROOT/"work/image_compose.spv",ROOT/"work/image_stats.spv")
        if path.is_file()}
    return out


def print_summary(case):
    width,height,scale=case["width"],case["height"],case["render_scale"]
    print(f"\n{width}x{height}, scale {scale:g}, network {case['network']}, exact outputs={case['exact_output_parity']}")
    print("mode                         total median/p95   prep   network   compose   metadata/history (ms)")
    for mode,result in case["modes"].items():
        stats=result["samples"]
        total=stats["device_total_ms" if mode=="gpu-device" else "daemon_total_ms"]
        med=lambda name:stats[name]["median"]
        print(f"{mode:28s} {total['median']:7.2f}/{total['p95']:7.2f} "
              f"{med('preprocess_ms'):7.2f} {med('network_ms'):8.2f} "
              f"{med('compose_ms'):9.2f} {med('metadata_history_ms'):9.2f}")


def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--case",type=case_spec,action="append",help="WIDTHxHEIGHT@SCALE; repeat for multiple cases")
    parser.add_argument("--warm",type=int,default=25)
    parser.add_argument("--samples",type=int,default=30)
    parser.add_argument("--minimum",type=int,default=320)
    parser.add_argument("--profile",choices=tuple(nr_frame.PROFILES),default="standard")
    parser.add_argument("--format",type=int,choices=(37,43,44,50),default=44)
    parser.add_argument("--output",type=pathlib.Path,default=ROOT/"work/gpu-frame-stages.json")
    parser.add_argument("--describe",action="store_true",help="show the plan without creating a GPU runtime")
    parser.add_argument("--smoke",action="store_true",help="one small warm/sample real-model frame per mode")
    parser.add_argument("--interleaved",action="store_true",help="warm all modes, then rotate their order per frame using retained contexts")
    parser.add_argument("--profile-network",action="store_true",help="after interleaved measurements, separately timestamp network passes in single-recording mode")
    parser.add_argument("--profile-samples",type=int,default=10)
    parser.add_argument("--driver",help="explicit Vulkan driver version/build reported by the probe; Windows CIM versions are also recorded")
    cli=parser.parse_args()
    if cli.smoke:
        cli.warm,cli.samples,cli.minimum=1,1,128
        cases=[(160,96,.5)]
    else:
        cases=cli.case or [(1280,720,.4),(1280,720,1.),(1920,1080,.4)]
    if cli.warm<1 or cli.samples<1 or cli.profile_samples<1 or not 128<=cli.minimum<=4096:
        parser.error("warm/samples must be positive; minimum must be 128..4096")
    if cli.profile_network and not cli.interleaved:
        parser.error("--profile-network requires --interleaved")
    plan=dict(modes=list(MODES),warmup_frames=cli.warm,measured_frames=cli.samples,
        interleaved=cli.interleaved,profile_network=cli.profile_network,
        format=cli.format,settings=dict(profile=cli.profile,intensity=1.,temporal=1.,hold=1.,
                                       release=24.,cut_limit=.15,min_extent=cli.minimum),cases=[])
    for width,height,scale in cases:
        iw,ih=nr_frame.render_extent(width,height,scale,cli.minimum)
        geometry=nr_frame.network_geometry(iw,ih,minimum=cli.minimum)
        plan["cases"].append(dict(width=width,height=height,render_scale=scale,inner=[iw,ih],
                                network=[geometry.network_width,geometry.network_height]))
    if cli.describe:
        print(json.dumps(plan,indent=2));return 0
    if nr_image.library() is None:
        raise SystemExit("the native CPU image library must be built for a fair baseline")
    if not nr_frame.WEIGHTS.is_file(): raise SystemExit(f"missing weights: {nr_frame.WEIGHTS}")
    # Match daemon startup, before this process creates its resident GPU context.
    D.check_half_rounding()
    keeping=nr_alloc if os.name=="nt" and nr_alloc.install() else None
    report=dict(metadata=metadata(),plan=plan,allocator_keeps_blocks=bool(keeping),smoke_only=cli.smoke,
        scope="daemon/image processing; synthetic fixture, in-memory control IO, no game/WSI capture",
        device_timing_excludes="fixture upload and diagnostic output readback; includes reduced metadata host reads",
        cases=[])
    if cli.driver: report["metadata"]["reported_vulkan_driver"]=cli.driver
    try:
        for described in plan["cases"]:
            width,height,scale=described["width"],described["height"],described["render_scale"]
            frames=fixture(width,height)
            case={**described,"fixture_sha256":[hashlib.sha256(frame).hexdigest() for frame in frames],"modes":{}}
            if cli.interleaved:
                print(f"interleaving {width}x{height}@{scale:g}: {cli.warm} warm + {cli.samples} samples per mode",flush=True)
                run_interleaved(case,frames,cli,keeping,report)
            for mode in (() if cli.interleaved else MODES):
                print(f"running {width}x{height}@{scale:g} {mode}: {cli.warm} warm + {cli.samples} samples",flush=True)
                backend=TracedBackend()
                args=live_args(width,height,scale,cli.minimum,cli.profile)
                report["metadata"].setdefault("device",xmx.device_name())
                report["metadata"].setdefault("memory",xmx.memory_note())
                report["metadata"].setdefault("graph_key",backend.runtime.graph_key())
                try:
                    if mode=="gpu-device":
                        result=run_device_mode(backend,args,frames,width,height,cli.format,cli.warm,cli.samples,keeping)
                    else:
                        result=run_daemon_mode(mode,backend,args,frames,width,height,cli.format,cli.warm,cli.samples,keeping)
                    case["modes"][mode]=result
                finally:
                    D.close_gpu_images(args);backend.close()
            hashes=[result["output_sha256"] for result in case["modes"].values()]
            case["exact_output_parity"]=all(value==hashes[0] for value in hashes[1:])
            report["cases"].append(case)
            cli.output.parent.mkdir(parents=True,exist_ok=True)
            cli.output.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
            print_summary(case)
            if not case["exact_output_parity"]:
                raise RuntimeError("benchmark paths returned different pixels; inspect saved hashes before comparing speed")
        print(f"\nSaved {cli.output}")
        return 0
    finally:
        if keeping: keeping.uninstall()


if __name__=="__main__": raise SystemExit(main())
