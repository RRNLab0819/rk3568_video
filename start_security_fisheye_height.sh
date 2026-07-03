#!/bin/sh
# Raw fisheye security demo:
# 2x2 raw camera view + raw YOLO boxes + 1.70m person-height distance label.

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn
CALIB_DIR=/userdata/calib

if [ ! -x "$BIN" ]; then
    echo "[fishheight] ERROR: binary not found or not executable: $BIN"
    exit 1
fi
if [ ! -f "$MODEL" ]; then
    echo "[fishheight] ERROR: model not found: $MODEL"
    exit 1
fi

NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[fishheight] NPU: governor=$(cat "$NPU_GOV") freq=$(cat "$NPU_FREQ") Hz"
fi

unset SECURITY_MODE
unset RECTIFIED_INFER
unset FISHEYE_MODE
unset OEM_AVM_MODE

export FISHEYE_CALIB_DIR="$CALIB_DIR"
export INFER_FLIPY="${INFER_FLIPY:-1,1,1,1}"
export SECURITY_PERSON_HEIGHT_M="${SECURITY_PERSON_HEIGHT_M:-1.70}"
export SECURITY_MAX_BOX_AGE_MS="${SECURITY_MAX_BOX_AGE_MS:-500}"
export SECURITY_RAW_CROP="${SECURITY_RAW_CROP:-0.80}"
export SECURITY_INFER_CONF="${SECURITY_INFER_CONF:-0.50}"
export SECURITY_PERSON_CONF="${SECURITY_PERSON_CONF:-0.50}"
export SECURITY_PERSIST="${SECURITY_PERSIST:-1}"
export SECURITY_DISPLAY_FPS="${SECURITY_DISPLAY_FPS:-25}"
export RK3568_CONFIG=/tmp/security_fisheye_height_config.ini

cat > "$RK3568_CONFIG" <<EOF
[camera]
count = 4
device = /dev/video
width  = 1920
height = 1080
fps    = 25

[encoder]
codec   = h265
bitrate = 4000000
gop     = 25

[inference]
enabled         = true
model           = $MODEL
interval        = 1
conf            = $SECURITY_INFER_CONF
nms             = 0.45
person_only     = true
person_conf     = $SECURITY_PERSON_CONF
smooth_enable   = false
smooth_alpha    = 0.70
min_persist     = $SECURITY_PERSIST
channels        = 0,1,2,3
round_robin     = true
rga_preprocess  = false

[display]
enabled  = true

[output]
pattern  = /tmp/cam_%d.h264
frames   = 0
EOF

pkill -INT rk3568_camera 2>/dev/null
sleep 1
pkill -9 rk3568_camera 2>/dev/null
killall -9 rk3568_camera 2>/dev/null
sleep 1

echo "[fishheight] model=$MODEL"
echo "[fishheight] raw fisheye view, crop=$SECURITY_RAW_CROP person_height=${SECURITY_PERSON_HEIGHT_M}m"
echo "[fishheight] conf=$SECURITY_INFER_CONF person_conf=$SECURITY_PERSON_CONF persist=$SECURITY_PERSIST infer_flipy=$INFER_FLIPY"
echo "[fishheight] launching $BIN ..."

cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 4 --no-enc

echo "[fishheight] exited"
