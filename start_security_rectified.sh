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

killall rk3568_camera 2>/dev/null
sleep 1

echo "[security] model=$MODEL"
echo "[security] calib=$CALIB_DIR"
echo "[security] extrinsics=$EXTRINSICS"
echo "[security] fov=$FISHEYE_FOV rot=$FISHEYE_ROTATE flipx=$FISHEYE_FLIPX flipy=$FISHEYE_FLIPY"
echo "[security] person_height=${SECURITY_PERSON_HEIGHT_M}m warn=${SECURITY_WARN_NEAR_M}/${SECURITY_WARN_MID_M}m"
echo "[security] rectified_infer=$RECTIFIED_INFER"
echo "[security] launching $BIN ..."

cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 4 --no-enc

echo "[security] exited"
