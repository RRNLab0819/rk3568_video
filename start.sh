#!/bin/sh
# RK3568 AI Camera - stable startup
# Usage: /userdata/start.sh

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5n_320.rknn

# ---- 1. Check model ----
if [ ! -f "$MODEL" ]; then
    echo "[start] ERROR: model not found: $MODEL"
    exit 1
fi

# ---- 2. Set NPU governor ----
NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[start] NPU: governor=$(cat $NPU_GOV) freq=$(cat $NPU_FREQ) Hz"
else
    echo "[start] WARNING: cannot set NPU governor"
fi

# ---- 3. Launch ----
echo "[start] model=$MODEL"
echo "[start] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib $BIN

echo "[start] exited"
