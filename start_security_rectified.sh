#!/bin/sh
# RK3568 Security Fisheye AI monitor.
# Four calibrated fisheye cameras + person detection + temporary height distance estimate.

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn
CALIB_DIR=/userdata/calib
EXTRINSICS=/userdata/calib/security_extrinsics.ini

if [ ! -x "$BIN" ]; then
    echo "[security] ERROR: binary not found or not executable: $BIN"
    exit 1
fi

if [ ! -f "$MODEL" ]; then
    echo "[security] ERROR: model not found: $MODEL"
    exit 1
fi

for cam in 0 1 2 3; do
    if [ ! -f "$CALIB_DIR/calib_video${cam}.yaml" ]; then
        echo "[security] ERROR: missing calibration: $CALIB_DIR/calib_video${cam}.yaml"
        exit 1
    fi
done

NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[security] NPU: governor=$(cat "$NPU_GOV") freq=$(cat "$NPU_FREQ") Hz"
else
    echo "[security] WARNING: cannot set NPU governor"
fi

if [ ! -f "$EXTRINSICS" ]; then
    cat > "$EXTRINSICS" <<'EOF'
# Reserved for final A-route ground-plane distance.
# Units: meters and degrees. Fill after camera mounting/extrinsic calibration.
[cam0]
height_m=1.20
pitch_deg=35.0
yaw_deg=0.0
roll_deg=0.0

[cam1]
height_m=1.20
pitch_deg=35.0
yaw_deg=90.0
roll_deg=0.0

[cam2]
height_m=1.20
pitch_deg=35.0
yaw_deg=180.0
roll_deg=0.0

[cam3]
height_m=1.20
pitch_deg=35.0
yaw_deg=270.0
roll_deg=0.0
EOF
fi

export SECURITY_MODE=1
export RECTIFIED_INFER=1
export FISHEYE_CALIB_DIR="$CALIB_DIR"
# 150 keeps more usable detail than the wider 160 view. Override when testing coverage.
export FISHEYE_FOV="${FISHEYE_FOV:-150,150,150,150}"
export FISHEYE_ROTATE="${FISHEYE_ROTATE:-0,0,0,0}"
export FISHEYE_FLIPX="${FISHEYE_FLIPX:-0,0,0,0}"
export FISHEYE_FLIPY="${FISHEYE_FLIPY:-0,0,0,0}"
export SECURITY_PERSON_HEIGHT_M="${SECURITY_PERSON_HEIGHT_M:-1.70}"
export SECURITY_WARN_NEAR_M="${SECURITY_WARN_NEAR_M:-1.50}"
export SECURITY_WARN_MID_M="${SECURITY_WARN_MID_M:-3.00}"
export SECURITY_EXTRINSICS="$EXTRINSICS"
export SECURITY_INFER_CONF="${SECURITY_INFER_CONF:-0.40}"
export SECURITY_PERSON_CONF="${SECURITY_PERSON_CONF:-0.30}"
export SECURITY_PERSIST="${SECURITY_PERSIST:-4}"
export RK3568_CONFIG=/tmp/security_rectified_config.ini

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
smooth_enable   = true
smooth_alpha    = 0.25
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

echo "[security] model=$MODEL"
echo "[security] calib=$CALIB_DIR"
echo "[security] extrinsics=$EXTRINSICS"
echo "[security] fov=$FISHEYE_FOV rot=$FISHEYE_ROTATE flipx=$FISHEYE_FLIPX flipy=$FISHEYE_FLIPY"
echo "[security] person_height=${SECURITY_PERSON_HEIGHT_M}m warn=${SECURITY_WARN_NEAR_M}/${SECURITY_WARN_MID_M}m"
echo "[security] camera_heights=${SECURITY_CAMERA_HEIGHTS:-from extrinsics} camera_pitches=${SECURITY_CAMERA_PITCHES:-from extrinsics}"
echo "[security] rectified_infer=$RECTIFIED_INFER"
echo "[security] config=$RK3568_CONFIG conf=$SECURITY_INFER_CONF person_conf=$SECURITY_PERSON_CONF persist=$SECURITY_PERSIST"
echo "[security] launching $BIN ..."

cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 4 --no-enc

echo "[security] exited"
