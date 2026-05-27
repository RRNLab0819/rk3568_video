#!/bin/sh
# RK3568 1-Channel + AI Person Detection
# Requires: /userdata/yolov5.rknn model file
# Usage: /userdata/start_ai_cam0.sh

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn

# ---- 1. Check model ----
if [ ! -f "$MODEL" ]; then
    echo "[ai] ERROR: model not found: $MODEL"
    echo "[ai] Please push the model file first."
    exit 1
fi

# ---- 2. Set NPU to performance ----
NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[ai] NPU: governor=$(cat $NPU_GOV) freq=$(cat $NPU_FREQ) Hz"
else
    echo "[ai] WARNING: cannot set NPU governor"
fi

# ---- 3. Launch with AI enabled ----
echo "[ai] model=$MODEL"
echo "[ai] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib $BIN -m "$MODEL" -c 1 --no-enc

echo "[ai] exited"
