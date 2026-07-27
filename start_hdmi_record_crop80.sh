#!/bin/sh
set -eu

ACTION="${1:-start}"
ROOT="${HDMI_REC_ROOT:-/userdata/hdmi_record_crop80}"
STATE="$ROOT/state.env"
LOG="$ROOT/record.log"
PID="$ROOT/record.pid"
RUN="$ROOT/record.run.sh"

CAM="${HDMI_REC_CAM:-0}"
WIDTH="${HDMI_REC_WIDTH:-1920}"
HEIGHT="${HDMI_REC_HEIGHT:-1080}"
FPS="${HDMI_REC_FPS:-15}"
BITRATE="${HDMI_REC_BITRATE:-4000000}"
GOP="${HDMI_REC_GOP:-15}"
CROP_PERCENT="${HDMI_REC_CROP_PERCENT:-80}"
SEGMENT_SEC="${HDMI_REC_SEGMENT_SEC:-60}"
MAX_FILES="${HDMI_REC_MAX_FILES:-0}"
OUT_BASE="${HDMI_REC_DIR:-}"
SD_MOUNT="${HDMI_REC_SD_MOUNT:-/mnt/sdcard}"

mkdir -p "$ROOT"

if [ "$CROP_PERCENT" -lt 50 ] || [ "$CROP_PERCENT" -gt 100 ]; then
  echo "[record] HDMI_REC_CROP_PERCENT must be between 50 and 100"
  exit 2
fi

CROP_MARGIN_X=$(( WIDTH * (100 - CROP_PERCENT) / 200 ))
CROP_MARGIN_Y=$(( HEIGHT * (100 - CROP_PERCENT) / 200 ))
SEGMENT_NS=$(( SEGMENT_SEC * 1000000000 ))

pick_record_dir() {
  if [ -n "$OUT_BASE" ]; then
    echo "$OUT_BASE"
    return 0
  fi

  if mount | awk '{print $3}' | grep -qx "$SD_MOUNT"; then
    echo "$SD_MOUNT/rk3568_recordings"
    return 0
  fi

  return 1
}

ensure_record_dir() {
  out="$1"
  mkdir -p "$out"
  if ! touch "$out/.write_test" 2>/dev/null; then
    echo "[record] cannot write to $out"
    exit 1
  fi
  rm -f "$out/.write_test"
}

kill_pid_file() {
  pf="$1"
  [ -f "$pf" ] || return 0
  pid="$(cat "$pf" 2>/dev/null || true)"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    sleep 3
  fi
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    sleep 2
  fi
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$pf"
}

stop_camera_users() {
  kill_pid_file "$PID"
  pkill -TERM rk3568_camera 2>/dev/null || true
  pkill -f start_webrtc_single.sh 2>/dev/null || true
  pkill -f "/userdata/webrtc_single/mediamtx" 2>/dev/null || true
  sleep 1
  for p in $(fuser "/dev/video$CAM" 2>/dev/null || true); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(fuser "/dev/video$CAM" 2>/dev/null || true); do
    kill -9 "$p" 2>/dev/null || true
  done
}

write_state() {
  cat > "$STATE" <<EOF
CAM=$CAM
WIDTH=$WIDTH
HEIGHT=$HEIGHT
FPS=$FPS
BITRATE=$BITRATE
GOP=$GOP
CROP_PERCENT=$CROP_PERCENT
CROP_MARGIN_X=$CROP_MARGIN_X
CROP_MARGIN_Y=$CROP_MARGIN_Y
SEGMENT_SEC=$SEGMENT_SEC
MAX_FILES=$MAX_FILES
SD_MOUNT=$SD_MOUNT
OUT_DIR=$OUT_DIR
EOF
}

load_state() {
  if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE"
  fi
}

status() {
  load_state
  echo "[record] stream: cam$CAM ${WIDTH}x${HEIGHT}@${FPS} bitrate=$BITRATE crop=${CROP_PERCENT}%"
  echo "[record] dir: ${OUT_DIR:-unknown}"
  echo "[record] log: $LOG"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[record] pipeline alive pid=$(cat "$PID")"
  else
    echo "[record] pipeline stopped"
  fi
  if [ -n "${OUT_DIR:-}" ] && [ -d "$OUT_DIR" ]; then
    ls -lh "$OUT_DIR" 2>/dev/null | tail -10 || true
  fi
}

start_recording() {
  OUT_DIR="$(pick_record_dir)" || {
    echo "[record] no SD card mount found. Expected $SD_MOUNT"
    exit 1
  }
  ensure_record_dir "$OUT_DIR"
  stop_camera_users
  write_state

  stamp="$(date +%Y%m%d_%H%M%S)"
  pattern="$OUT_DIR/cam${CAM}_${stamp}_%05d.mp4"
  : > "$LOG"
cat > "$RUN" <<EOF
#!/bin/sh
exec gst-launch-1.0 -e \\
  v4l2src device="/dev/video$CAM" io-mode=mmap do-timestamp=true \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! videocrop left=$CROP_MARGIN_X right=$CROP_MARGIN_X top=$CROP_MARGIN_Y bottom=$CROP_MARGIN_Y \\
  ! videoscale \\
  ! videorate drop-only=true \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! tee name=t \\
    t. ! queue leaky=downstream max-size-buffers=2 max-size-time=0 max-size-bytes=0 \\
       ! waylandsink fullscreen=true sync=false qos=true \\
    t. ! queue max-size-buffers=10 max-size-time=0 max-size-bytes=0 \\
       ! mpph264enc bps=$BITRATE bps-min=$BITRATE bps-max=$BITRATE rc-mode=cbr profile=baseline max-pending=1 gop=$GOP header-mode=1 \\
       ! h264parse config-interval=1 \\
       ! splitmuxsink location="$pattern" muxer-factory=mp4mux async-finalize=true max-size-time=$SEGMENT_NS max-files=$MAX_FILES send-keyframe-requests=true >>"$LOG" 2>&1
EOF
  chmod +x "$RUN"
  nohup "$RUN" >/dev/null 2>&1 </dev/null &
  echo $! > "$PID"
  sleep 5

  if ! kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    if [ "$FPS" = "25" ]; then
      echo "[record] 25fps pipeline exited early, retrying at 15fps"
      FPS=15
      GOP=15
      start_recording
      return 0
    fi
  fi

  status
}

case "$ACTION" in
  start)
    start_recording
    ;;
  stop|stop-all)
    stop_camera_users
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -120 "$LOG" 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop|stop-all|status|logs]"
    echo "env: HDMI_REC_CAM=0 HDMI_REC_FPS=25 HDMI_REC_CROP_PERCENT=80 HDMI_REC_DIR=/mnt/sdcard/rk3568_recordings"
    exit 2
    ;;
esac
