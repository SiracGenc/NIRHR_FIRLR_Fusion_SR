
#Dependences
"""
sudo apt update
sudo apt install -y \
  python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
  gir1.2-gst-rtsp-server-1.0 \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  gstreamer1.0-libcamera \
  v4l-utils
"""
#!/usr/bin/env python3
import os
import glob
import argparse

import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Gst, GstRtspServer, Gtk  # noqa: E402

Gst.init(None)


def list_v4l2_devices():
    devs = []
    for p in sorted(glob.glob("/sys/class/video4linux/video*")):
        dev = "/dev/" + os.path.basename(p)
        name_path = os.path.join(p, "name")
        try:
            with open(name_path, "r", encoding="utf-8") as f:
                name = f.read().strip()
        except Exception:
            name = "(unknown)"
        devs.append((dev, name))
    return devs


def clamp_int(x, lo, hi, default):
    try:
        v = int(x)
    except Exception:
        return default
    return max(lo, min(hi, v))


class DualRtspGui(Gtk.Window):
    def __init__(self, port: int, gs_path: str, th_path: str):
        super().__init__(title="Dual RTSP (GS + Thermal)")

        self.port = port
        self.gs_path = gs_path if gs_path.startswith("/") else "/" + gs_path
        self.th_path = th_path if th_path.startswith("/") else "/" + th_path

        self.server = GstRtspServer.RTSPServer()
        # bind all interfaces (ethernet/wifi)
        self.server.props.address = "0.0.0.0"
        self.server.props.service = str(self.port)

        self.mounts = self.server.get_mount_points()

        self._build_ui()
        self._mount_initial_factories()

        self.server.attach(None)

    def _build_ui(self):
        self.set_border_width(10)
        self.set_default_size(520, 380)

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.add(outer)

        info = Gtk.Label(
            label=f"RTSP: rtsp://<PI_IP>:{self.port}{self.gs_path}   and   rtsp://<PI_IP>:{self.port}{self.th_path}\n"
                  f"改参数后：对“新连接”生效（建议客户端断开重连）。"
        )
        info.set_xalign(0.0)
        outer.pack_start(info, False, False, 0)

        grid = Gtk.Grid(column_spacing=10, row_spacing=8)
        outer.pack_start(grid, True, True, 0)

        # -------- GS controls --------
        gs_frame = Gtk.Frame(label="GS (libcamerasrc)")
        gs_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        gs_box.set_border_width(8)
        gs_frame.add(gs_box)

        self.gs_w = Gtk.Entry(text="1280")
        self.gs_h = Gtk.Entry(text="720")
        self.gs_fps = Gtk.Entry(text="30")
        self.gs_bitrate = Gtk.Entry(text="4000")  # kbps

        gs_box.pack_start(self._row("Width", self.gs_w, "px"), False, False, 0)
        gs_box.pack_start(self._row("Height", self.gs_h, "px"), False, False, 0)
        gs_box.pack_start(self._row("FPS", self.gs_fps, ""), False, False, 0)
        gs_box.pack_start(self._row("Bitrate", self.gs_bitrate, "kbps"), False, False, 0)

        # -------- Thermal controls --------
        th_frame = Gtk.Frame(label="Thermal (v4l2src)")
        th_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        th_box.set_border_width(8)
        th_frame.add(th_box)

        self.th_dev_combo = Gtk.ComboBoxText()
        self._refresh_v4l2_combo()

        self.th_w = Gtk.Entry(text="640")
        self.th_h = Gtk.Entry(text="480")
        self.th_fps = Gtk.Entry(text="9")
        self.th_bitrate = Gtk.Entry(text="1000")  # kbps

        th_box.pack_start(self._row("Device", self.th_dev_combo, ""), False, False, 0)
        th_box.pack_start(self._row("Scale Width", self.th_w, "px"), False, False, 0)
        th_box.pack_start(self._row("Scale Height", self.th_h, "px"), False, False, 0)
        th_box.pack_start(self._row("FPS", self.th_fps, ""), False, False, 0)
        th_box.pack_start(self._row("Bitrate", self.th_bitrate, "kbps"), False, False, 0)

        grid.attach(gs_frame, 0, 0, 1, 1)
        grid.attach(th_frame, 1, 0, 1, 1)

        # Buttons
        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        outer.pack_start(btn_row, False, False, 0)

        self.btn_refresh = Gtk.Button(label="Refresh /dev/video*")
        self.btn_refresh.connect("clicked", lambda _b: self._refresh_v4l2_combo())
        btn_row.pack_start(self.btn_refresh, True, True, 0)

        self.btn_apply = Gtk.Button(label="Apply (new clients)")
        self.btn_apply.connect("clicked", lambda _b: self.apply_settings())
        btn_row.pack_start(self.btn_apply, True, True, 0)

        self.btn_quit = Gtk.Button(label="Quit")
        self.btn_quit.connect("clicked", lambda _b: Gtk.main_quit())
        btn_row.pack_start(self.btn_quit, True, True, 0)

    def _row(self, key: str, widget, suffix: str):
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        lab = Gtk.Label(label=key)
        lab.set_xalign(0.0)
        lab.set_size_request(110, -1)
        row.pack_start(lab, False, False, 0)

        row.pack_start(widget, True, True, 0)

        if suffix:
            s = Gtk.Label(label=suffix)
            s.set_xalign(0.0)
            s.set_size_request(50, -1)
            row.pack_start(s, False, False, 0)

        return row

    def _refresh_v4l2_combo(self):
        self.th_dev_combo.remove_all()
        devs = list_v4l2_devices()
        if not devs:
            self.th_dev_combo.append_text("/dev/video0 (not found in sysfs)")
            self.th_dev_combo.set_active(0)
            return
        for dev, name in devs:
            self.th_dev_combo.append_text(f"{dev}  —  {name}")
        self.th_dev_combo.set_active(0)

    def _selected_thermal_dev(self) -> str:
        txt = self.th_dev_combo.get_active_text() or "/dev/video0"
        return txt.split("—")[0].strip()

    def _build_gs_launch(self, w: int, h: int, fps: int, bitrate_kbps: int) -> str:
        bitrate = max(100, bitrate_kbps) * 1000
        # NV12 -> H264 (try HW v4l2 encoder)
        return (
            f"( "
            f"libcamerasrc ! video/x-raw,width={w},height={h},framerate={fps}/1,format=NV12 "
            f"! queue "
            f"! v4l2h264enc extra-controls=\"controls,video_bitrate={bitrate},repeat_sequence_header=1\" "
            f"! h264parse config-interval=1 "
            f"! rtph264pay name=pay0 pt=96 config-interval=1 "
            f")"
        )

    def _build_th_launch(self, dev: str, w: int, h: int, fps: int, bitrate_kbps: int) -> str:
        bitrate = max(100, bitrate_kbps) * 1000
        # Accept whatever raw comes from /dev/videoX, convert/scale to I420 then encode
        return (
            f"( "
            f"v4l2src device={dev} ! video/x-raw,framerate={fps}/1 "
            f"! queue "
            f"! videoconvert "
            f"! videoscale "
            f"! video/x-raw,width={w},height={h},format=I420 "
            f"! queue "
            f"! v4l2h264enc extra-controls=\"controls,video_bitrate={bitrate},repeat_sequence_header=1\" "
            f"! h264parse config-interval=1 "
            f"! rtph264pay name=pay0 pt=96 config-interval=1 "
            f")"
        )

    def _set_factory(self, path: str, launch: str):
        factory = GstRtspServer.RTSPMediaFactory()
        factory.set_launch(launch)
        factory.set_shared(True)

        # replace existing
        try:
            self.mounts.remove_factory(path)
        except Exception:
            pass
        self.mounts.add_factory(path, factory)

    def _mount_initial_factories(self):
        self.apply_settings()

    def apply_settings(self):
        gs_w = clamp_int(self.gs_w.get_text(), 16, 7680, 1280)
        gs_h = clamp_int(self.gs_h.get_text(), 16, 4320, 720)
        gs_fps = clamp_int(self.gs_fps.get_text(), 1, 240, 30)
        gs_br = clamp_int(self.gs_bitrate.get_text(), 100, 200000, 4000)

        th_dev = self._selected_thermal_dev()
        th_w = clamp_int(self.th_w.get_text(), 16, 7680, 640)
        th_h = clamp_int(self.th_h.get_text(), 16, 4320, 480)
        th_fps = clamp_int(self.th_fps.get_text(), 1, 60, 9)
        th_br = clamp_int(self.th_bitrate.get_text(), 100, 200000, 1000)

        gs_launch = self._build_gs_launch(gs_w, gs_h, gs_fps, gs_br)
        th_launch = self._build_th_launch(th_dev, th_w, th_h, th_fps, th_br)

        self._set_factory(self.gs_path, gs_launch)
        self._set_factory(self.th_path, th_launch)

        print("Updated factories:")
        print(" GS     :", self.gs_path, gs_launch)
        print(" THERMAL:", self.th_path, th_launch)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--gs-path", type=str, default="/gs")
    ap.add_argument("--th-path", type=str, default="/thermal")
    args = ap.parse_args()

    win = DualRtspGui(args.port, args.gs_path, args.th_path)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()