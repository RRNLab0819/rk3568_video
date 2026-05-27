# Runbook

## Build

```bash
source /home/rrn/3568/3568_sdk/environment-setup
cd /home/rrn/rk3568-camera
make clean && make
```

## Deploy

```bash
adb push rk3568_camera /userdata/rk3568_camera
adb shell chmod +x /userdata/rk3568_camera
adb push config.ini /userdata/rk3568-camera/config.ini
```

## Run Commands

### Baseline display (no AI, no correction)

```bash
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera
# or: ./rk3568_camera --no-enc
```

Shows 4 cameras in 2x2 grid.

### Single camera

```bash
./rk3568_camera -c 1 --no-enc          # cam0 only
./rk3568_camera -c 1 --cam 2 --no-enc  # cam2 only
```

### AI person detection (cam0)

```bash
./rk3568_camera -m /userdata/yolov5.rknn -c 1 --no-enc --rga
```

Red boxes overlay on display. Person-only by default.

### 4-camera + AI

```bash
./rk3568_camera -m /userdata/yolov5.rknn -c 4 --rga
```

Round-robin inference across all cameras.

### Fisheye correction

```bash
# 2x2 grid with per-camera fisheye undistort
FISHEYE_MODE=1 ./rk3568_camera -c 4 --no-enc

# Single camera fullscreen debug
FISHEYE_MODE=1 FISHEYE_DEBUG_CAM=0 ./rk3568_camera -c 4 --no-enc

# Custom FOV (comma-separated per camera)
FISHEYE_MODE=1 FISHEYE_FOV=124,155,161,170 ./rk3568_camera -c 4 --no-enc

# Rotation/flip
FISHEYE_MODE=1 FISHEYE_ROTATE=0,90,0,0 FISHEYE_FLIPX=0,1,0,0 ./rk3568_camera -c 4 --no-enc

# Live frame dump
FISHEYE_MODE=1 FISHEYE_DEBUG_CAM=0 FISHEYE_LIVE_DUMP=1 ./rk3568_camera -c 4 --no-enc
```

### AVM surround-view layout

```bash
AVM_MODE=1 ./rk3568_camera -c 4 --no-enc
```

Left sidebar + vehicle placeholder + 4 corrected fisheye views.

## NPU Performance

```bash
# Check governor
cat /sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/governor

# Set performance mode
echo performance > /sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/governor
```

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| "Device or resource busy" | `killall rk3568_camera grab_frame; sleep 2` |
| Black screen on start | Check Wayland: `ps | grep weston` |
| Low FPS | Check NPU governor, close other apps |
| ADB offline after display test | Known Mali/Wayland ADB issue; reboot board |
| Fisheye black borders | Adjust per-camera FOV via `FISHEYE_FOV=...` |
| Inference not working | Check model file exists, `--rga` flag, NPU governor |

## Config File

`/userdata/rk3568-camera/config.ini`:

```ini
[camera]
count = 4
width  = 1920
height = 1080
fps    = 25

[encoder]
codec   = h265
bitrate = 4000000

[inference]
enabled = false
model   = /userdata/yolov5.rknn
interval = 1
conf    = 0.20
nms     = 0.45

[display]
enabled  = true
```
