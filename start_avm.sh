#!/bin/sh
# RK3568 OEM AVM UI, 4-channel AI display, encoder disabled by default.
# Requires: /userdata/yolov5.rknn

BIN=/userdata/rk3568_camera
MODEL=/userdata/yolov5.rknn

if [ ! -x "$BIN" ]; then
    echo "[avm] ERROR: binary not found or not executable: $BIN"
    exit 1
fi

if [ ! -f "$MODEL" ]; then
    echo "[avm] ERROR: model not found: $MODEL"
    exit 1
fi

NPU_GOV=/sys/class/devfreq/fde40000.npu/governor
NPU_FREQ=/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq
if [ -w "$NPU_GOV" ]; then
    echo performance > "$NPU_GOV" 2>/dev/null
    echo "[avm] NPU: governor=$(cat "$NPU_GOV") freq=$(cat "$NPU_FREQ") Hz"
else
    echo "[avm] WARNING: cannot set NPU governor"
fi

export OEM_AVM_MODE=1
export OEM_AVM_VIEW=${OEM_AVM_VIEW:-surround-main}
export OEM_AVM_MAIN_CAM=${OEM_AVM_MAIN_CAM:-0}
export OEM_AVM_CAM_MAP=${OEM_AVM_CAM_MAP:-0,1,2,3}

echo "[avm] model=$MODEL"
echo "[avm] view=$OEM_AVM_VIEW main_cam=$OEM_AVM_MAIN_CAM cam_map=$OEM_AVM_CAM_MAP"
echo "[avm] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib "$BIN" -m "$MODEL" -c 4 --no-enc

echo "[avm] exited"
