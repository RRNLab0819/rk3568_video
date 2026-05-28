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

## Recommended Board Commands

Run from the RK3568 board.

```bash
# Four-camera AI display, no encoder. Recommended for normal debugging.
/userdata/start_ai.sh

# Four-camera AI display + H.265 encoder.
/userdata/start_ai_enc.sh

# OEM AVM UI prototype, no encoder.
/userdata/start_avm.sh

# OEM AVM UI prototype + H.265 encoder.
/userdata/start_avm_enc.sh
```

The RKNN model path should be:

```bash
/userdata/yolov5.rknn
```

## Direct Commands

```bash
cd /userdata

# Baseline 4-camera grid
LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -c 4 --no-enc

# 4-camera AI grid
LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc

# 4-camera AI grid + encoder
LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5.rknn -c 4

# OEM AVM UI prototype
OEM_AVM_MODE=1 LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc

# OEM AVM UI prototype + encoder
OEM_AVM_MODE=1 LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5.rknn -c 4
```

## Runtime Notes

- `--no-enc` disables MPP encoding and usually gives the most stable display/debug experience.
- Enabling encoder adds MPP and DDR bandwidth pressure. Lower FPS compared with no-encode mode is expected.
- `OEM_AVM_MODE=1` enables the current AVM UI prototype. It is not a calibrated 360 bird-view stitcher yet.
- Do not rely on `AVM_MODE=1` for current builds; use `OEM_AVM_MODE=1` or the provided scripts.

## NPU Performance

```bash
# Check governor
cat /sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/governor

# Set performance mode
echo performance > /sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/governor
```

## Troubleshooting

| Symptom | Action |
|---------|--------|
| `Device or resource busy` | `killall rk3568_camera grab_frame; sleep 2` |
| Black screen on start | Check Wayland/Weston process and HDMI output |
| AI does not start | Check `/userdata/yolov5.rknn` exists and NPU governor is performance |
| FPS drops after enabling encoder | Expected load increase; compare with `--no-enc` |
| AVM 1/2 pages do not look like real 360 | Expected current limitation; formal calibration/IPM is not complete |
| Detection box has wrong geometry in future BEV view | Needs calibrated mapping; raw image boxes cannot be directly projected to BEV |

## Config File

Typical board config path:

```bash
/userdata/rk3568-camera/config.ini
```

Important defaults:

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
conf    = 0.40
nms     = 0.45

[display]
enabled = true
```
