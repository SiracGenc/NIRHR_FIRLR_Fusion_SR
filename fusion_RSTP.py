#!/usr/bin/env python3
import time
import threading
import argparse
import numpy as np
import cv2

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GstRtspServer, GLib

Gst.init(None)

def ns():
    return time.monotonic_ns()

class FusionState:
    def __init__(self, alpha: float, delta_ms: float):
        self.alpha = float(alpha)
        self.delta_ns = int(delta_ms * 1e6)

        self.lock = threading.Lock()
        self.last_th_frame = None          # np.ndarray BGR
        self.last_th_ts = None             # ns

        self.appsrc = None                # set when RTSP media created
        self.out_w = None
        self.out_h = None
        self.out_fps = None

    def update_thermal(self, frame_bgr: np.ndarray, ts_ns: int):
        with self.lock:
            self.last_th_frame = frame_bgr
            self.last_th_ts = ts_ns

    def fuse_and_push(self, gs_bgr: np.ndarray, gs_ts_ns: int):
        with self.lock:
            th = self.last_th_frame
            th_ts = self.last_th_ts
            appsrc = self.appsrc
            out_w, out_h, out_fps = self.out_w, self.out_h, self.out_fps

        if appsrc is None or out_w is None:
            return  # no client yet

        # resize GS to output size (normally out==gs size)
        if gs_bgr.shape[1] != out_w or gs_bgr.shape[0] != out_h:
            gs_bgr = cv2.resize(gs_bgr, (out_w, out_h), interpolation=cv2.INTER_LINEAR)

        if th is None:
            fused = gs_bgr
        else:
            # hold last thermal if no new frame; use ±Δt only as "simultaneous" label (optional)
            if th.shape[1] != out_w or th.shape[0] != out_h:
                th2 = cv2.resize(th, (out_w, out_h), interpolation=cv2.INTER_LINEAR)
            else:
                th2 = th

            # linear blend
            a = self.alpha
            fused = cv2.addWeighted(gs_bgr, 1.0 - a, th2, a, 0.0)

            # optional: if you want to visualize stale thermal
            if th_ts is not None and abs(gs_ts_ns - th_ts) > self.delta_ns:
                cv2.putText(fused, "TH STALE", (20, 40),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 255), 2)

        # push to appsrc as BGR
        data = fused.tobytes()
        buf = Gst.Buffer.new_allocate(None, len(data), None)
        buf.fill(0, data)

        # timestamp: use monotonic-based running time; simplest is do-timestamp on appsrc,
        # but you can also set buf.pts explicitly if you build a running-time mapping.
        # (GstBuffer PTS is standard metadata.) :contentReference[oaicite:5]{index=5}

        appsrc.emit("push-buffer", buf)

class FusionFactory(GstRtspServer.RTSPMediaFactory):
    def __init__(self, state: FusionState, width: int, height: int, fps: int, bitrate_kbps: int):
        super().__init__()
        self.state = state
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate_kbps = bitrate_kbps

        launch = (
            f"( appsrc name=fsrc is-live=true format=time do-timestamp=true "
            f"caps=video/x-raw,format=BGR,width={width},height={height},framerate={fps}/1 "
            f"! videoconvert ! video/x-raw,format=I420 "
            f"! x264enc tune=zerolatency speed-preset=ultrafast bitrate={bitrate_kbps} key-int-max={fps} "
            f"! h264parse config-interval=1 "
            f"! rtph264pay name=pay0 pt=96 config-interval=1 )"
        )
        self.set_launch(launch)
        self.set_shared(True)  # one pipeline shared by all clients :contentReference[oaicite:6]{index=6}

    def do_configure(self, media):
        # get appsrc and store into shared state
        e = media.get_element()
        appsrc = e.get_by_name("fsrc")
        self.state.appsrc = appsrc
        self.state.out_w = self.width
        self.state.out_h = self.height
        self.state.out_fps = self.fps

def make_appsinks(gs_w, gs_h, gs_fps, th_dev):
    # appsink: emit-signals must be true to get new-sample callbacks :contentReference[oaicite:7]{index=7}
    gs_pipe = Gst.parse_launch(
        f"libcamerasrc ! video/x-raw,width={gs_w},height={gs_h},framerate={gs_fps}/1,format=NV12 "
        f"! videoconvert ! video/x-raw,format=BGR "
        f"! appsink name=gssink emit-signals=true max-buffers=1 drop=true sync=false"
    )
    th_pipe = Gst.parse_launch(
        f"v4l2src device={th_dev} do-timestamp=true "
        f"! video/x-raw,format=RGB,width=160,height=120 "
        f"! videoconvert ! video/x-raw,format=BGR "
        f"! appsink name=thsink emit-signals=true max-buffers=1 drop=true sync=false"
    )
    return gs_pipe, th_pipe

def sample_to_bgr(sample):
    buf = sample.get_buffer()
    caps = sample.get_caps()
    s = caps.get_structure(0)
    w = s.get_value("width")
    h = s.get_value("height")

    ok, mapinfo = buf.map(Gst.MapFlags.READ)
    if not ok:
        return None
    try:
        arr = np.frombuffer(mapinfo.data, dtype=np.uint8).reshape((h, w, 3))
        return arr.copy()
    finally:
        buf.unmap(mapinfo)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--path", type=str, default="/fusion")
    ap.add_argument("--gs-w", type=int, default=1280)
    ap.add_argument("--gs-h", type=int, default=720)
    ap.add_argument("--gs-fps", type=int, default=30)
    ap.add_argument("--th-dev", type=str, default="/dev/video42")
    ap.add_argument("--alpha", type=float, default=0.35)
    ap.add_argument("--delta-ms", type=float, default=50.0)
    ap.add_argument("--bitrate-kbps", type=int, default=4000)
    args = ap.parse_args()

    state = FusionState(args.alpha, args.delta_ms)

    # RTSP server
    server = GstRtspServer.RTSPServer()
    server.props.address = "0.0.0.0"
    server.props.service = str(args.port)
    mounts = server.get_mount_points()

    factory = FusionFactory(state, args.gs_w, args.gs_h, args.gs_fps, args.bitrate_kbps)
    mounts.add_factory(args.path, factory)
    server.attach(None)

    # capture pipelines -> appsinks
    gs_pipe, th_pipe = make_appsinks(args.gs_w, args.gs_h, args.gs_fps, args.th_dev)
    gssink = gs_pipe.get_by_name("gssink")
    thsink = th_pipe.get_by_name("thsink")

    def on_th_sample(sink):
        sample = sink.emit("pull-sample")
        frame = sample_to_bgr(sample)
        if frame is not None:
            state.update_thermal(frame, ns())
        return Gst.FlowReturn.OK

    def on_gs_sample(sink):
        sample = sink.emit("pull-sample")
        frame = sample_to_bgr(sample)
        if frame is not None:
            state.fuse_and_push(frame, ns())
        return Gst.FlowReturn.OK

    thsink.connect("new-sample", on_th_sample)
    gssink.connect("new-sample", on_gs_sample)

    th_pipe.set_state(Gst.State.PLAYING)
    gs_pipe.set_state(Gst.State.PLAYING)

    print(f"Fusion RTSP: rtsp://<PI_IP>:{args.port}{args.path}")
    loop = GLib.MainLoop()
    loop.run()

if __name__ == "__main__":
    main()