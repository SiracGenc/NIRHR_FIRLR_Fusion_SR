# NIRHR_FIRLR_Fusion_SR

> Beginner-friendly dual-camera demo for Raspberry Pi: **Global Shutter (NIR/visible style high-FPS stream)** + **Lepton thermal stream** with optional RTSP output.
>
> 面向入门者的树莓派双相机项目：**全局快门（高速可见光/NIR）** + **Lepton 热成像**，并支持 RTSP 推流。

---

## Language / 语言切换

- [English](#english)
- [中文](#中文)

---

## English

### 1. What this repo does

This project contains two practical scripts for Raspberry Pi camera experiments:

1. `gs_demo.py`: a local preview and recording demo for a high-speed Global Shutter camera (60 FPS target).
2. `RTSP.py`: a GUI tool that publishes **two RTSP streams at the same time**:
   - GS stream from `libcamerasrc`
   - Thermal stream from `v4l2src` (typically a Lepton/v4l2loopback pipeline)

It is designed for undergraduate students with EE/CS background: you can quickly run it first, then optimize quality/latency step by step.

---

### 2. Recommended hardware/software environment

- Raspberry Pi 4/5 (or equivalent with CSI + USB/SPI support)
- Raspberry Pi OS (Bookworm recommended)
- A Global Shutter camera supported by `picamera2/libcamera`
- A thermal source available as `/dev/video*` (for example: FLIR Lepton via `v4l2lepton` + loopback)

---

### 3. Dependency installation

> If you are using Raspberry Pi OS, install system packages first, then Python packages.

#### 3.1 System packages

```bash
sudo apt update
sudo apt install -y \
  python3-pip python3-opencv python3-gi python3-gi-cairo \
  gir1.2-gstreamer-1.0 gir1.2-gst-rtsp-server-1.0 gir1.2-gtk-3.0 \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  ffmpeg
```

#### 3.2 Python packages

```bash
python3 -m pip install --upgrade pip
python3 -m pip install numpy picamera2
```

> Notes:
>
> - `RTSP.py` requires PyGObject (`gi`) + GStreamer RTSP bindings.
> - `gs_demo.py` requires `opencv-python` APIs and `picamera2`.

---

### 4. Quick start

#### 4.1 Global Shutter local demo

```bash
python3 gs_demo.py
```

Keyboard controls in the preview window:

- `Q` / `Esc`: quit
- `R`: start/stop recording (`.mp4`)
- `S`: save snapshot (`.png`)
- `+` / `-`: increase/decrease bitrate (applied to new recording)

#### 4.2 Dual RTSP GUI streaming

```bash
python3 RTSP.py --port 8554 --gs-path /gs --th-path /thermal
```

Then open stream URLs from VLC/ffplay:

- `rtsp://<PI_IP>:8554/gs`
- `rtsp://<PI_IP>:8554/thermal`

---

### 5. CLI options explanation (`RTSP.py`)

| Option | Default | Meaning |
|---|---:|---|
| `--port` | `8554` | RTSP server port |
| `--gs-path` | `/gs` | Mount path for Global Shutter stream |
| `--th-path` | `/thermal` | Mount path for thermal stream |

Inside the GUI, you can further set width/height/FPS/bitrate for both streams, and select thermal input device from detected `/dev/video*` entries.

---

### 6. Functional mechanism (how it works)

#### 6.1 `gs_demo.py` mechanism

- Creates a `Picamera2` video configuration at **1456×1088 @ 60 FPS**.
- Locks AE (`AeEnable=False`) and fixes exposure to improve 60 FPS stability.
- Uses OpenCV to draw a real-time OSD panel (FPS, exposure, temperature, bitrate, mode).
- Optional recording uses `H264Encoder + FfmpegOutput`.

#### 6.2 `RTSP.py` mechanism

- Builds a `GstRtspServer.RTSPServer` and mounts two factories.
- GS pipeline:
  `libcamerasrc -> videoconvert -> H.264 encoder -> rtph264pay`
- Thermal pipeline:
  `v4l2src -> videoconvert -> videorate -> videoscale -> x264enc -> rtph264pay`
- Encoder fallback logic for GS:
  prefers `v4l2h264enc` when available, else falls back to `x264enc`.
- Parameter updates are applied to **new client connections** (best practice: reconnect players after clicking Apply).

---

### 7. Typical usage examples

#### Example A: low-latency classroom demo

```bash
python3 RTSP.py --port 8554 --gs-path /gs --th-path /th
```

- In GUI: set GS to `1280x720 @ 30 FPS`, thermal to `160x120 @ 9 FPS`.
- In VLC: open both URLs and tile view for side-by-side comparison.

#### Example B: high-quality local GS capture

```bash
python3 gs_demo.py
```

- Press `+` several times to increase bitrate before recording.
- Press `R` to record a short clip for motion analysis.

---

### 8. Common troubleshooting

1. **`ModuleNotFoundError: gi`**
   - Install `python3-gi` and `gir1.2-*` packages again.
2. **No thermal device in dropdown**
   - Check `/dev/video*` and your Lepton/v4l2loopback pipeline.
3. **RTSP opens but no image**
   - Verify codec plugins (`ugly/libav`) and test with `gst-inspect-1.0 x264enc`.
4. **GS FPS unstable**
   - Reduce resolution, close heavy background tasks, and monitor Pi temperature.

---

### 9. Third-party note

The folder `Thirdparty/v4l2lepton_by_groupgets_modified/` contains Lepton-related code and tools that can be integrated as the thermal source provider.

---

## 中文

### 1. 这个仓库是做什么的

这个项目主要提供两类脚本，适合树莓派上的双模态相机实验：

1. `gs_demo.py`：全局快门相机的本地预览与录制（目标 60 FPS）。
2. `RTSP.py`：一个图形化工具，可同时发布两路 RTSP：
   - 来自 `libcamerasrc` 的 GS 画面
   - 来自 `v4l2src` 的热成像画面（通常来自 Lepton/v4l2loopback）

面向有 EE/CS 基础的本科生：先跑通，再逐步优化画质、码率和时延。

---

### 2. 建议环境

- Raspberry Pi 4/5（或同级别，支持 CSI 与 USB/SPI）
- Raspberry Pi OS（推荐 Bookworm）
- 支持 `picamera2/libcamera` 的全局快门相机
- 能在 `/dev/video*` 暴露热成像流的设备（例如 Lepton + `v4l2lepton` + loopback）

---

### 3. 依赖安装

> 建议顺序：先安装系统包，再安装 Python 包。

#### 3.1 系统依赖

```bash
sudo apt update
sudo apt install -y \
  python3-pip python3-opencv python3-gi python3-gi-cairo \
  gir1.2-gstreamer-1.0 gir1.2-gst-rtsp-server-1.0 gir1.2-gtk-3.0 \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  ffmpeg
```

#### 3.2 Python 依赖

```bash
python3 -m pip install --upgrade pip
python3 -m pip install numpy picamera2
```

补充说明：

- `RTSP.py` 依赖 PyGObject（`gi`）和 GStreamer RTSP 绑定。
- `gs_demo.py` 依赖 OpenCV 和 `picamera2`。

---

### 4. 快速开始

#### 4.1 全局快门本地演示

```bash
python3 gs_demo.py
```

窗口内快捷键：

- `Q` / `Esc`：退出
- `R`：开始/停止录制（输出 `.mp4`）
- `S`：保存截图（输出 `.png`）
- `+` / `-`：增减码率（对新录制生效）

#### 4.2 双路 RTSP 图形化推流

```bash
python3 RTSP.py --port 8554 --gs-path /gs --th-path /thermal
```

然后在 VLC/ffplay 中打开：

- `rtsp://<树莓派IP>:8554/gs`
- `rtsp://<树莓派IP>:8554/thermal`

---

### 5. 参数解释（`RTSP.py`）

| 参数 | 默认值 | 作用 |
|---|---:|---|
| `--port` | `8554` | RTSP 服务端口 |
| `--gs-path` | `/gs` | 全局快门流的挂载路径 |
| `--th-path` | `/thermal` | 热成像流的挂载路径 |

在 GUI 中，还可以继续配置两路流的分辨率、帧率、码率，并从自动扫描到的 `/dev/video*` 里选择热成像输入设备。

---

### 6. 机制解释（核心原理）

#### 6.1 `gs_demo.py` 工作机制

- 使用 `Picamera2` 创建 **1456×1088 @ 60 FPS** 视频配置。
- 关闭自动曝光并固定曝光时间，提高 60 FPS 下的稳定性。
- 使用 OpenCV 绘制工业风叠加信息（FPS、曝光、温度、码率、录制状态）。
- 录制链路采用 `H264Encoder + FfmpegOutput`。

#### 6.2 `RTSP.py` 工作机制

- 通过 `GstRtspServer.RTSPServer` 同时挂载两路媒体工厂。
- GS 链路：
  `libcamerasrc -> videoconvert -> H.264 编码 -> rtph264pay`
- 热成像链路：
  `v4l2src -> videoconvert -> videorate -> videoscale -> x264enc -> rtph264pay`
- GS 编码器有自动回退策略：
  优先用 `v4l2h264enc`（硬编），不可用时回退 `x264enc`（软编）。
- 点击 Apply 后，参数通常对**新连接客户端**生效（建议播放器断开重连）。

---

### 7. 使用样例

#### 示例 A：课堂演示（低时延、稳定）

```bash
python3 RTSP.py --port 8554 --gs-path /gs --th-path /th
```

- GUI 中建议：GS `1280x720 @ 30 FPS`，热成像 `160x120 @ 9 FPS`。
- VLC 同时打开两路地址，做并排对比。

#### 示例 B：本地高质量录制

```bash
python3 gs_demo.py
```

- 录制前多按几次 `+` 提高码率。
- 按 `R` 录一段短视频用于后续运动分析。

---

### 8. 常见问题排查

1. **报错 `ModuleNotFoundError: gi`**
   - 重新安装 `python3-gi` 与 `gir1.2-*` 相关包。
2. **热成像设备下拉框空白**
   - 检查 `/dev/video*` 是否存在以及 Lepton/loopback 是否正常。
3. **RTSP 能连上但没画面**
   - 检查编码插件是否齐全，先用 `gst-inspect-1.0 x264enc` 验证。
4. **GS 帧率波动较大**
   - 适当降低分辨率，关闭后台重负载任务，关注温度。

---

### 9. 第三方代码说明

`Thirdparty/v4l2lepton_by_groupgets_modified/` 目录提供了与 Lepton 相关的代码与工具，可用于构建热成像输入链路。
