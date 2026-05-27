#!/bin/sh
# RK3568 4-Channel Display Only (no AI inference)
# Usage: /userdata/start_display.sh

BIN=/userdata/rk3568_camera

echo "[display] 4-channel display mode (AI disabled)"
echo "[display] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib $BIN

echo "[display] exited"
