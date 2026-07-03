#!/bin/sh
# Experimental low-latency security mode:
# raw fisheye inference + calibrated footpoint distance + corrected display.

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn
CALIB_DIR=/userdata/calib
EXTRINSICS=/userdata/calib/security_extrinsics.ini

if [ ! -x "$BIN" ]; then
    echo "[rawfoot] ERROR: binary not found or not executable: $BIN"
    exit 1
fi
if [ ! -f "$MODEL" ]; then
    echo "[rawfoot] ERROR: model not found: $MODEL"
    exit 1
fi
for cam in 0 1 2 3; do
    if [ ! -f "$CALIB_DIR/calib_video${cam}.yaml" ]; then
        echo "[rawfoot] ERROR: missing calibration: $CALIB_DIR/calib_video${cam}.yaml"
        exit 1
    fi
done

NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[rawfoot] NPU: governor=$(cat "$NPU_GOV") freq=$(cat "$NPU_FREQ") Hz"
fi

if [ ! -f "$EXTRINSICS" ]; then
    cat > "$EXTRINSICS" <<'EOF'
[cam0]
height_m=1.20
pitch_deg=35.0
[cam1]
height_m=1.20
pitch_deg=35.0
[cam2]
height_m=1.20
pitch_deg=35.0
[cam3]
height_m=1.20
pitch_deg=35.0
EOF
fi

export SECURITY_MODE=1
export RECTIFIED_INFER=0
export SECURITY_FOOTPOINT_ONLY=1
export SECURITY_MAX_BOX_AGE_MS="${SECURITY_MAX_BOX_AGE_MS:-350}"
export SECURITY_DISPLAY_FPS="${SECURITY_DISPLAY_FPS:-25}"
export FISHEYE_CALIB_DIR="$CALIB_DIR"
export FISHEYE_FOV="${FISHEYE_FOV:-150,150,150,150}"
export FISHEYE_ROTATE="${FISHEYE_ROTATE:-0,0,0,0}"
export FISHEYE_FLIPX="${FISHEYE_FLIPX:-0,0,0,0}"
export FISHEYE_FLIPY="${FISHEYE_FLIPY:-0,0,0,0}"
export INFER_FLIPY="${INFER_FLIPY:-1,1,1,1}"
export SECURITY_PERSON_HEIGHT_M="${SECURITY_PERSON_HEIGHT_M:-1.70}"
export SECURITY_WARN_NEAR_M="${SECURITY_WARN_NEAR_M:-1.50}"
export SECURITY_WARN_MID_M="${SECURITY_WARN_MID_M:-3.00}"
export SECURITY_EXTRINSICS="$EXTRINSICS"
export SECURITY_INFER_CONF="${SECURITY_INFER_CONF:-0.50}"
export SECURITY_PERSON_CONF="${SECURITY_PERSON_CONF:-0.50}"
export SECURITY_PERSIST="${SECURITY_PERSIST:-1}"
export RK3568_CONFIG=/tmp/security_rawfoot_config.ini

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

echo "[rawfoot] model=$MODEL"
echo "[rawfoot] calib=$CALIB_DIR extrinsics=$EXTRINSICS"
echo "[rawfoot] fov=$FISHEYE_FOV footpoint_distance=$SECURITY_FOOTPOINT_ONLY max_age=${SECURITY_MAX_BOX_AGE_MS}ms display_fps=$SECURITY_DISPLAY_FPS infer_flipy=$INFER_FLIPY"
echo "[rawfoot] conf=$SECURITY_INFER_CONF person_conf=$SECURITY_PERSON_CONF persist=$SECURITY_PERSIST rectified_infer=$RECTIFIED_INFER"
echo "[rawfoot] launching $BIN ..."

cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 4 --no-enc

echo "[rawfoot] exited"
