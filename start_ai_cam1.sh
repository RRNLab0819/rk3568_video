#!/bin/sh
# RK3568 /dev/video1 single-channel AI view for calibration capture
# Requires: /userdata/yolov5.rknn model file
# Usage: /userdata/start_ai_cam1.sh

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn
CAM=1

# ---- 1. Check model ----
if [ ! -f "$MODEL" ]; then
    echo "[ai-cam$CAM] ERROR: model not found: $MODEL"
    echo "[ai-cam$CAM] Please push the model file first."
    exit 1
fi

# ---- 2. Set NPU to performance ----
NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[ai-cam$CAM] NPU: governor=$(cat $NPU_GOV) freq=$(cat $NPU_FREQ) Hz"
else
    echo "[ai-cam$CAM] WARNING: cannot set NPU governor"
fi

# ---- 3. Launch one physical camera with AI enabled ----
echo "[ai-cam$CAM] model=$MODEL"
echo "[ai-cam$CAM] physical device=/dev/video$CAM"
echo "[ai-cam$CAM] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 1 --cam "$CAM" --no-enc

echo "[ai-cam$CAM] exited"
