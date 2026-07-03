# Runbook

## Current Recommended Board Command

Run from the RK3568 board:

```bash
cd /userdata
./start_security_fisheye_height.sh
```

This is the current accepted demo:

- 4 camera raw fisheye 2x2 view
- center crop 80%
- RKNN YOLOv5 person detection
- raw-coordinate boxes
- compact distance labels based on 1.70m person-height estimate
- no encoder by default

## Build

In the Linux VM:

```bash
source /home/rrn/3568/3568_sdk/environment-setup
cd /home/rrn/rk3568-camera
make clean && make
```

## Deploy

```bash
adb push rk3568_camera /userdata/rk3568_camera
adb shell chmod +x /userdata/rk3568_camera
adb push start_security_fisheye_height.sh /userdata/start_security_fisheye_height.sh
adb shell chmod +x /userdata/start_security_fisheye_height.sh
```

The RKNN model path should be:

```bash
/userdata/yolov5.rknn
```

Calibration files should be under:

```bash
/userdata/calib/calib_video0.yaml
/userdata/calib/calib_video1.yaml
/userdata/calib/calib_video2.yaml
/userdata/calib/calib_video3.yaml
```

## Useful Runtime Overrides

```bash
# Less crop, more fisheye edge visible
SECURITY_RAW_CROP=0.90 ./start_security_fisheye_height.sh

# More strict person confidence
SECURITY_INFER_CONF=0.55 SECURITY_PERSON_CONF=0.55 ./start_security_fisheye_height.sh

# Change assumed person height for the demo distance estimate
SECURITY_PERSON_HEIGHT_M=1.75 ./start_security_fisheye_height.sh
```

## Other Board Commands

```bash
# Four-camera AI display, no encoder.
/userdata/start_ai.sh

# Four-camera AI display + H.265 encoder.
/userdata/start_ai_enc.sh

# Rectified-image direct inference experiment.
/userdata/start_security_rectified.sh

# Raw footpoint/extrinsics experiment.
/userdata/start_security_rawfoot.sh

# OEM AVM UI prototype, no encoder.
/userdata/start_avm.sh

# OEM AVM UI prototype + H.265 encoder.
/userdata/start_avm_enc.sh
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
```

## Runtime Notes

- `--no-enc` disables MPP encoding and usually gives the most stable display/debug experience.
- Enabling encoder adds MPP and DDR bandwidth pressure. Lower FPS compared with no-encode mode is expected.
- Current recommended security demo uses raw fisheye display because box alignment is more reliable than projecting raw boxes onto a rectified view.
- `SECURITY_RAW_CROP=0.80` improves visual appearance by hiding the most distorted outer edge, but it also hides people at the cropped border.
- `OEM_AVM_MODE=1` enables the current AVM UI prototype. It is not a calibrated 360 bird-view stitcher yet.

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
| Script reports `/bin/sh^M` or bad interpreter | Convert script to LF line endings and push again |
| Black screen on start | Check Wayland/Weston process and HDMI output |
| AI does not start | Check `/userdata/yolov5.rknn` exists and NPU governor is performance |
| FPS drops after enabling encoder | Expected load increase; compare with `--no-enc` |
| Person box is clipped near image edge | Expected if `SECURITY_RAW_CROP` hides the outer raw image area |
| AVM 1/2 pages do not look like real 360 | Expected current limitation; formal calibration/IPM is not complete |

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
