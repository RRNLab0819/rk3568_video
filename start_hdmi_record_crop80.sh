#!/bin/sh
set -eu

ACTION="${1:-start}"
ROOT="${HDMI_REC_ROOT:-/userdata/hdmi_record_crop80}"
STATE="$ROOT/state.env"
LOG="$ROOT/record.log"
PID="$ROOT/record.pid"
RUN="$ROOT/record.run.sh"
BIN="${HDMI_REC_BIN:-/userdata/hdmi_record_switcher}"
FOREGROUND="${HDMI_REC_FOREGROUND:-0}"
REC_TZ="${HDMI_REC_TZ:-CST-8}"

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
DUAL="${HDMI_REC_DUAL:-1}"

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

detect_keyboard_event() {
  for mode in strict fallback; do
    name=""
    while IFS= read -r line; do
      case "$line" in
        N:\ Name=*) name="$line" ;;
        H:\ Handlers=*)
          match=0
          case "$mode:$name" in
            strict:*Keyboard*) match=1 ;;
            fallback:*SONiX\ USB\ DEVICE*|fallback:*USB\ DEVICE*|fallback:*SEMICO*|fallback:*Logitech*|fallback:*2.4G*) match=1 ;;
          esac
          if [ "$match" = "1" ]; then
            case "$line" in
              *kbd*event*)
                for tok in $line; do
                  case "$tok" in
                    event*) echo "/dev/input/$tok"; return 0 ;;
                  esac
                done
                ;;
            esac
          fi
          ;;
      esac
    done < /proc/bus/input/devices
  done
  return 0
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
  pkill -INT hdmi_record_switcher 2>/dev/null || true
  sleep 2
  pkill -TERM hdmi_record_switcher 2>/dev/null || true
  pkill -TERM rk3568_camera 2>/dev/null || true
  pkill -f start_webrtc_single.sh 2>/dev/null || true
  pkill -f "/userdata/webrtc_single/mediamtx" 2>/dev/null || true
  sleep 1
  pkill -KILL hdmi_record_switcher 2>/dev/null || true
  for p in $(fuser "/dev/video$CAM" 2>/dev/null || true); do
    kill "$p" 2>/dev/null || true
  done
  if [ "$DUAL" = "1" ]; then
    for p in $(fuser /dev/video1 2>/dev/null || true); do
      kill "$p" 2>/dev/null || true
    done
  fi
  sleep 1
  for p in $(fuser "/dev/video$CAM" 2>/dev/null || true); do
    kill -9 "$p" 2>/dev/null || true
  done
  if [ "$DUAL" = "1" ]; then
    for p in $(fuser /dev/video1 2>/dev/null || true); do
      kill -9 "$p" 2>/dev/null || true
    done
  fi
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
DUAL=$DUAL
REC_TZ=$REC_TZ
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
  if [ "${DUAL:-0}" = "1" ]; then
    echo "[record] dual: cam0 + cam1, HDMI keys 1=cam0 2=cam1 3=latest"
  fi
  echo "[record] dir: ${OUT_DIR:-unknown}"
  echo "[record] log: $LOG"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[record] pipeline alive pid=$(cat "$PID")"
  else
    echo "[record] pipeline stopped"
  fi
  if [ -n "${OUT_DIR:-}" ] && [ -d "$OUT_DIR" ]; then
    find "$OUT_DIR" -type f 2>/dev/null | sort | tail -10 | while read -r f; do
      ls -lh "$f" 2>/dev/null || true
    done
  fi
}

start_recording() {
  if [ "$DUAL" = "1" ]; then
    OUT_DIR="$SD_MOUNT/rk3568_recordings"
    if ! mount | awk '{print $3}' | grep -qx "$SD_MOUNT"; then
      echo "[record] no SD card mount found. Expected $SD_MOUNT"
      exit 1
    fi
    ensure_record_dir "$OUT_DIR"
    stop_camera_users
    write_state
    : > "$LOG"
    if ! [ -x "$BIN" ]; then
      echo "[record] missing executable: $BIN"
      echo "[record] build with: make hdmi_record_switcher"
      exit 1
    fi

    EVENT_ARG="${HDMI_REC_EVENT:-}"
    if [ -z "$EVENT_ARG" ]; then
      EVENT_ARG="$(detect_keyboard_event)"
    fi
    if [ -n "$EVENT_ARG" ]; then
      echo "[record] keyboard event: $EVENT_ARG"
    else
      echo "[record] keyboard event: none detected; set HDMI_REC_EVENT=/dev/input/eventX or HDMI_REC_FOREGROUND=1"
    fi

    if [ "$FOREGROUND" = "1" ]; then
      env \
        HDMI_REC_WIDTH="$WIDTH" HDMI_REC_HEIGHT="$HEIGHT" HDMI_REC_FPS="$FPS" \
        HDMI_REC_BITRATE="$BITRATE" HDMI_REC_CROP_PERCENT="$CROP_PERCENT" \
        HDMI_REC_SEGMENT_SEC="$SEGMENT_SEC" HDMI_REC_MAX_FILES="$MAX_FILES" \
        HDMI_REC_ROOT="$OUT_DIR" HDMI_REC_SD_MOUNT="$SD_MOUNT" HDMI_REC_EVENT="$EVENT_ARG" HDMI_REC_TZ="$REC_TZ" \
        "$BIN" 2>&1 | tee "$LOG"
      return 0
    fi

    nohup env \
      HDMI_REC_WIDTH="$WIDTH" HDMI_REC_HEIGHT="$HEIGHT" HDMI_REC_FPS="$FPS" \
      HDMI_REC_BITRATE="$BITRATE" HDMI_REC_CROP_PERCENT="$CROP_PERCENT" \
      HDMI_REC_SEGMENT_SEC="$SEGMENT_SEC" HDMI_REC_MAX_FILES="$MAX_FILES" \
      HDMI_REC_ROOT="$OUT_DIR" HDMI_REC_SD_MOUNT="$SD_MOUNT" HDMI_REC_EVENT="$EVENT_ARG" HDMI_REC_TZ="$REC_TZ" \
      "$BIN" >"$LOG" 2>&1 </dev/null &
    echo $! > "$PID"
    sleep 5
    status
    return 0
  fi

  OUT_DIR="$(pick_record_dir)" || {
    echo "[record] no SD card mount found. Expected $SD_MOUNT"
    exit 1
  }
  ensure_record_dir "$OUT_DIR"
  stop_camera_users
  write_state

  stamp="$(TZ="$REC_TZ" date +%Y%m%d_%H%M%S)"
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
