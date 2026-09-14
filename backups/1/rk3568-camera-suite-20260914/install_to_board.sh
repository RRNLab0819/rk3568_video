#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TARGET=/userdata
STAMP=$(date +%Y%m%d-%H%M%S)
[ -d /mnt/sdcard ] && mount | awk '{print $3}' | grep -qx /mnt/sdcard || {
  echo "[install] mounted SD card required at /mnt/sdcard"
  exit 1
}
BACKUP=/mnt/sdcard/rk3568-preinstall-backups/$STAMP

[ "$(uname -m)" = aarch64 ] || { echo "[install] aarch64 target required"; exit 1; }
for dev in /dev/video0 /dev/video1 /dev/video2; do
  [ -c "$dev" ] || { echo "[install] missing $dev"; exit 1; }
done
for cmd in ffmpeg gst-launch-1.0 hostapd dnsmasq wpa_supplicant wpa_passphrase udhcpc httpd ntpd iw; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "[install] missing command: $cmd"; exit 1; }
done
(cd "$HERE" && sha256sum -c SHA256SUMS.txt)

mkdir -p "$BACKUP" "$TARGET/camera_service_runtime" "$TARGET/camera_pipeline_runtime"
[ ! -x "$TARGET/camera_service.sh" ] || "$TARGET/camera_service.sh" stop >/dev/null 2>&1 || true
[ ! -x "$TARGET/start_complete_work.sh" ] || "$TARGET/start_complete_work.sh" stop >/dev/null 2>&1 || true

for path in \
  /userdata/camera_service.sh \
  /userdata/camera_pipeline.sh \
  /userdata/camera_engine \
  /userdata/camera_service_runtime \
  /userdata/camera_pipeline_runtime \
  /userdata/start_complete_work.sh \
  /userdata/start_hdmi_record_crop80.sh \
  /userdata/hdmi_record_switcher \
  /userdata/camera_channels.conf \
  /userdata/recording_retention.sh \
  /userdata/mediamtx \
  /userdata/webrtc_single \
  /userdata/media-server \
  /userdata/wifi-manager \
  /etc/init.d/S99rk3568-wifi \
  /etc/init.d/S99zz-rk3568-camera \
  /etc/apache2/httpd.conf \
  /etc/wpa_supplicant.conf \
  /usr/htdocs/wifi; do
  [ ! -e "$path" ] || cp -a --parents "$path" "$BACKUP"
done

# The old names were copied to the SD rollback directory above. Remove only
# those exact legacy paths before extracting the renamed release, which also
# avoids temporarily filling the small /userdata partition.
rm -f \
  /userdata/start_complete_work.sh \
  /userdata/start_hdmi_record_crop80.sh \
  /userdata/hdmi_record_switcher
rm -rf \
  /userdata/complete_work \
  /userdata/hdmi_record_crop80 \
  /userdata/webrtc_single

tar -xzf "$HERE/payload/userdata.tgz" -C /userdata
tar -xzf "$HERE/payload/system-config.tgz" -C /

chmod +x \
  /userdata/camera_service.sh \
  /userdata/camera_pipeline.sh \
  /userdata/camera_engine \
  /userdata/recording_retention.sh \
  /userdata/mediamtx/mediamtx \
  /userdata/media-server/media-server-arm64 \
  /userdata/wifi-manager/wifi_manager.sh \
  /etc/init.d/S99rk3568-wifi \
  /etc/init.d/S99zz-rk3568-camera

if [ -f "$HERE/saved-wifi.conf" ]; then
  cp "$HERE/saved-wifi.conf" /userdata/wifi-manager/wpa_supplicant.conf
  cp "$HERE/saved-wifi.conf" /etc/wpa_supplicant.conf
  chmod 600 /userdata/wifi-manager/wpa_supplicant.conf /etc/wpa_supplicant.conf
fi

# Disable the vendor AVM launcher recoverably when present; it competes for cameras and CPU.
if [ -e /etc/init.d/S50avm ]; then
  mkdir -p /userdata/disabled-init
  mv /etc/init.d/S50avm "/userdata/disabled-init/S50avm.disabled-$STAMP"
fi

# Preserve a static maintenance address, but remove the known dead default gateway
# that otherwise overrides the Wi-Fi default route on some target images.
if [ -f /etc/network/interfaces ] && grep -q 'gateway[[:space:]]\+172\.18\.17\.16' /etc/network/interfaces; then
  cp -a /etc/network/interfaces "$BACKUP/etc-network-interfaces"
  sed -i '/^[[:space:]]*gateway[[:space:]]\+172\.18\.17\.16[[:space:]]*$/d' /etc/network/interfaces
  ip route del default via 172.18.17.16 dev eth0 2>/dev/null || true
fi

httpd -t
httpd -k graceful 2>/dev/null || true
/etc/init.d/S99rk3568-wifi restart
/etc/init.d/S99zz-rk3568-camera start
sync

echo "[install] installed; rollback backup: $BACKUP"
echo "[install] start/status command: /userdata/camera_service.sh"
echo "[install] camera startup waits for Wi-Fi, valid time, and /mnt/sdcard"
