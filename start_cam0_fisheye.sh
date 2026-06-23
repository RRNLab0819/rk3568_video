#!/bin/sh
# RK3568 cam0 fisheye correction test
# Uses /userdata/calib/calib_video0.yaml generated from video0/cam0 calibration.

BIN=/userdata/rk3568_camera
CALIB_DIR=/userdata/calib
NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq

if [ ! -x "$BIN" ]; then
    echo "[fisheye] ERROR: binary not found: $BIN"
    exit 1
fi
if [ ! -f "$CALIB_DIR/calib_video0.yaml" ]; then
    echo "[fisheye] ERROR: calibration not found: $CALIB_DIR/calib_video0.yaml"
    exit 1
fi
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[fisheye] NPU: governor=$(cat $NPU_GOV) freq=$(cat $NPU_FREQ) Hz"
fi

killall rk3568_camera 2>/dev/null
sleep 1

echo "[fisheye] using $CALIB_DIR/calib_video0.yaml"
echo "[fisheye] launching cam0 fisheye debug view"
cd /userdata && \
FISHEYE_MODE=1 \
FISHEYE_DEBUG_CAM=0 \
FISHEYE_CALIB_DIR="$CALIB_DIR" \
FISHEYE_FOV=110,155,161,170 \
LD_LIBRARY_PATH=/usr/lib \
"$BIN" -c 1 --no-enc

echo "[fisheye] exited"
