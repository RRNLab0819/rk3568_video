#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REMOTE=/userdata/rk3568_camera_suite_installer
WITH_WIFI=0

[ "${1:-}" != "--with-saved-wifi" ] || WITH_WIFI=1
command -v adb >/dev/null 2>&1 || { echo "[deploy] adb not found"; exit 1; }
count=$(adb devices | awk 'NR>1 && $2=="device"{n++} END{print n+0}')
[ "$count" -eq 1 ] || { echo "[deploy] exactly one adb device is required (found $count)"; exit 1; }
[ "$(adb shell uname -m | tr -d '\r')" = aarch64 ] || { echo "[deploy] aarch64 target required"; exit 1; }

cd "$HERE"
sha256sum -c SHA256SUMS.txt
adb shell "mkdir -p '$REMOTE/payload'"
adb push payload/userdata.tgz "$REMOTE/payload/userdata.tgz"
adb push payload/system-config.tgz "$REMOTE/payload/system-config.tgz"
adb push install_to_board.sh "$REMOTE/install_to_board.sh"
adb push SHA256SUMS.txt "$REMOTE/SHA256SUMS.txt"

if [ "$WITH_WIFI" -eq 1 ]; then
  [ -f private/wpa_supplicant.conf ] || { echo "[deploy] private Wi-Fi config missing"; exit 1; }
  adb push private/wpa_supplicant.conf "$REMOTE/saved-wifi.conf"
fi

adb shell "cd '$REMOTE' && chmod +x install_to_board.sh && ./install_to_board.sh"
