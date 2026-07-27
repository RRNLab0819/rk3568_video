#!/bin/sh
set -eu

ACTION="${1:-start}"

ROOT="${COMPLETE_ROOT:-/userdata/complete_work}"
WEBRTC_ROOT="${WEBRTC_ROOT:-/userdata/webrtc_single}"
MEDIAMTX="${MEDIAMTX_BIN:-$WEBRTC_ROOT/mediamtx}"
CONF="$ROOT/mediamtx.yml"
MTX_PID="$ROOT/mediamtx.pid"
MTX_LOG="$ROOT/mediamtx.log"

MEDIA_SERVER_DIR="${MEDIA_SERVER_DIR:-/userdata/media-server}"
MEDIA_SERVER_BIN="${MEDIA_SERVER_BIN:-$MEDIA_SERVER_DIR/media-server-arm64}"
MEDIA_SERVER_CONFIG="${MEDIA_SERVER_CONFIG:-configs/config.yaml}"
MEDIA_SERVER_PID="$ROOT/media-server.pid"
MEDIA_SERVER_LOG="$ROOT/media-server.log"

REC_SCRIPT="${HDMI_REC_SCRIPT:-/userdata/start_hdmi_record_crop80.sh}"
REC_ROOT="${HDMI_REC_ROOT:-/userdata/hdmi_record_crop80}"
REC_SD_MOUNT="${HDMI_REC_SD_MOUNT:-/mnt/sdcard}"
REC_OUT_DIR="${HDMI_REC_DIR:-$REC_SD_MOUNT/rk3568_recordings}"

WIDTH="${HDMI_REC_WIDTH:-1920}"
HEIGHT="${HDMI_REC_HEIGHT:-1080}"
FPS="${HDMI_REC_FPS:-15}"
BITRATE="${HDMI_REC_BITRATE:-4000000}"
GOP="${HDMI_REC_GOP:-15}"
CROP_PERCENT="${HDMI_REC_CROP_PERCENT:-80}"
SEGMENT_SEC="${HDMI_REC_SEGMENT_SEC:-60}"
REC_TZ="${HDMI_REC_TZ:-CST-8}"
PAUSE_RECOVERY="${COMPLETE_PAUSE_RECOVERY:-1}"
TIME_MIN_YEAR="${COMPLETE_TIME_MIN_YEAR:-2026}"
TIME_WAIT_SEC="${COMPLETE_TIME_WAIT_SEC:-60}"

mkdir -p "$ROOT"

board_ip() {
  ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1
}

wait_valid_time() {
  i=0
  while [ "$i" -lt "$TIME_WAIT_SEC" ]; do
    year="$(date +%Y 2>/dev/null || echo 1970)"
    if [ "$year" -ge "$TIME_MIN_YEAR" ] 2>/dev/null; then
      echo "[complete] system time ok: $(date)"
      return 0
    fi
    echo "[complete] waiting for valid system time: $(date) year=$year min=$TIME_MIN_YEAR"
    sleep 1
    i=$((i + 1))
  done

  echo "[complete] system time is still invalid after ${TIME_WAIT_SEC}s: $(date)"
  echo "[complete] refusing to record into a wrong date directory"
  echo "[complete] fix NTP/system time, or set COMPLETE_TIME_MIN_YEAR=1970 only for debugging"
  exit 1
}

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return 0
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[complete] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[complete] recovery resumed pid=$pid"
  fi
}

stop_media_server() {
  if [ -f "$MEDIA_SERVER_PID" ]; then
    pid="$(cat "$MEDIA_SERVER_PID" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$MEDIA_SERVER_PID"
  fi
  pkill -f "$MEDIA_SERVER_BIN -config $MEDIA_SERVER_CONFIG" 2>/dev/null || true
}

stop_mediamtx() {
  if [ -f "$MTX_PID" ]; then
    pid="$(cat "$MTX_PID" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$MTX_PID"
  fi
  pkill -f "$MEDIAMTX $CONF" 2>/dev/null || true
}

start_media_server() {
  if ! [ -x "$MEDIA_SERVER_BIN" ]; then
    echo "[complete] missing media-server executable: $MEDIA_SERVER_BIN"
    exit 1
  fi
  if ! [ -f "$MEDIA_SERVER_DIR/$MEDIA_SERVER_CONFIG" ]; then
    echo "[complete] missing media-server config: $MEDIA_SERVER_DIR/$MEDIA_SERVER_CONFIG"
    exit 1
  fi

  stop_media_server
  : > "$MEDIA_SERVER_LOG"
  (
    cd "$MEDIA_SERVER_DIR"
    exec ./media-server-arm64 -config "$MEDIA_SERVER_CONFIG"
  ) >"$MEDIA_SERVER_LOG" 2>&1 &
  echo $! > "$MEDIA_SERVER_PID"
  sleep 1
}

start_mediamtx() {
  if ! [ -x "$MEDIAMTX" ]; then
    echo "[complete] missing MediaMTX executable: $MEDIAMTX"
    echo "[complete] put mediamtx at $MEDIAMTX or set MEDIAMTX_BIN=/path/to/mediamtx"
    exit 1
  fi

  ip="$(board_ip)"
  [ -n "$ip" ] || ip="127.0.0.1"
  cat > "$CONF" <<EOF
logLevel: info
rtsp: true
rtspAddress: :8554
rtmp: false
hls: false
webrtc: true
webrtcAddress: :8889
webrtcAllowOrigins: ['*']
webrtcAdditionalHosts: [$ip]
srt: false
api: false
metrics: false
pprof: false
paths:
  all_others:
EOF

  if ! pgrep -f "$MEDIAMTX $CONF" >/dev/null 2>&1; then
    pkill -f "/userdata/.*/mediamtx" 2>/dev/null || true
    : > "$MTX_LOG"
    nohup "$MEDIAMTX" "$CONF" >"$MTX_LOG" 2>&1 </dev/null &
    echo $! > "$MTX_PID"
    sleep 2
  fi
}

status() {
  ip="$(board_ip)"
  [ -n "$ip" ] || ip="127.0.0.1"
  echo "[complete] HDMI display: keys 1=cam0 2=cam1 3=latest"
  echo "[complete] record: cam0 + cam1 ${WIDTH}x${HEIGHT}@${FPS} crop=${CROP_PERCENT}% -> $REC_OUT_DIR"
  echo "[complete] WebRTC viewer cam0: http://$ip:8889/cam0/"
  echo "[complete] WebRTC viewer cam1: http://$ip:8889/cam1/"
  echo "[complete] WHEP cam0: http://$ip:8889/cam0/whep"
  echo "[complete] WHEP cam1: http://$ip:8889/cam1/whep"
  echo "[complete] media-server: http://$ip:8866"
  if [ -f "$MTX_PID" ] && kill -0 "$(cat "$MTX_PID" 2>/dev/null)" 2>/dev/null; then
    echo "[complete] mediamtx alive pid=$(cat "$MTX_PID") log=$MTX_LOG"
  else
    echo "[complete] mediamtx stopped log=$MTX_LOG"
  fi
  if [ -f "$MEDIA_SERVER_PID" ] && kill -0 "$(cat "$MEDIA_SERVER_PID" 2>/dev/null)" 2>/dev/null; then
    echo "[complete] media-server alive pid=$(cat "$MEDIA_SERVER_PID") log=$MEDIA_SERVER_LOG"
  else
    echo "[complete] media-server stopped log=$MEDIA_SERVER_LOG"
  fi
  "$REC_SCRIPT" status 2>/dev/null || true
}

case "$ACTION" in
  start)
    if ! mount | awk '{print $3}' | grep -qx "$REC_SD_MOUNT"; then
      echo "[complete] no SD card mount found: $REC_SD_MOUNT"
      exit 1
    fi
    wait_valid_time
    pause_recovery
    stop_media_server
    "$REC_SCRIPT" stop >/dev/null 2>&1 || true
    start_mediamtx
    HDMI_REC_WEBRTC=1 \
    HDMI_REC_KEEP_MEDIAMTX=1 \
    HDMI_REC_RTSP_BASE=rtsp://127.0.0.1:8554 \
    HDMI_REC_DUAL=1 \
    HDMI_REC_ROOT="$REC_ROOT" \
    HDMI_REC_SD_MOUNT="$REC_SD_MOUNT" \
    HDMI_REC_DIR="$REC_OUT_DIR" \
    HDMI_REC_WIDTH="$WIDTH" \
    HDMI_REC_HEIGHT="$HEIGHT" \
    HDMI_REC_FPS="$FPS" \
    HDMI_REC_GOP="$GOP" \
    HDMI_REC_BITRATE="$BITRATE" \
    HDMI_REC_CROP_PERCENT="$CROP_PERCENT" \
    HDMI_REC_SEGMENT_SEC="$SEGMENT_SEC" \
    HDMI_REC_TZ="$REC_TZ" \
    "$REC_SCRIPT" start
    start_media_server
    status
    ;;
  stop|stop-all)
    stop_media_server
    "$REC_SCRIPT" stop >/dev/null 2>&1 || true
    stop_mediamtx
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -120 "$MTX_LOG" "$REC_ROOT/record.log" "$MEDIA_SERVER_LOG" 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop|stop-all|status|logs]"
    exit 2
    ;;
esac
