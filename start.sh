#!/bin/sh
# RK3568 Camera - stable display launcher (AI disabled by default)
# Usage: /userdata/start.sh
#
# This is the baseline display-only startup.
# For AI inference testing, use: /userdata/start_ai_cam0.sh

BIN=/userdata/rk3568_camera

echo "[start] display-only mode (AI disabled)"
echo "[start] launching $BIN ..."
cd /userdata && LD_LIBRARY_PATH=/usr/lib $BIN

echo "[start] exited"
