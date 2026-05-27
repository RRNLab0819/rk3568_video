# RK3568 Multi-Channel AI Security Camera

Hardware: RK3568 + 4x fisheye cameras (1920x1080 NV12 @25fps) + Mali G52 GPU + NPU

## Capabilities

| Feature | Status | Description |
|---------|--------|-------------|
| 4-ch V4L2 capture | Done | /dev/video0-3, 1920x1080 NV12, dma_buf mmap |
| 2x2 quad display | Done | Wayland + EGL + GLES2, zero-copy YUV->RGB shader |
| MPP H.265 encode | Done | Per-camera encoder, /tmp/cam_%d.h264 |
| RKNN YOLOv5 inference | Done | Person-only detection, RGA dma_buf preprocess option |
| Fisheye mesh undistort | Done | Per-camera Kannala-Brandt model, chessboard calibrated |
| AVM layout display | Done | Sidebar + vehicle placeholder + 4 corrected views |

## Quick Start

```bash
# Baseline 4-camera display
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera

# Single camera
./rk3568_camera -c 1 --no-enc

# AI inference (camera 0 only)
./rk3568_camera -m /userdata/yolov5.rknn -c 1 --no-enc --rga

# Fisheye correction (all 4 cameras)
FISHEYE_MODE=1 ./rk3568_camera -c 4 --no-enc

# AVM surround-view layout
AVM_MODE=1 ./rk3568_camera -c 4 --no-enc
```

## Build

Requires RK3568 Buildroot SDK at `/home/rrn/3568/3568_sdk`.

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make
adb push rk3568_camera /userdata/
```

## Files

```
src/           C/C++ source (capture, display, encoder, inference, pipeline, fisheye)
tools/         Python tools (verify, calibrate), C tools (grab_frame, RGA test)
docs/          Documentation
config.ini     Runtime configuration
start*.sh      Launch scripts
```
