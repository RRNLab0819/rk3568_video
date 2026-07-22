#!/bin/sh
set -eu

ROOT=/userdata/rtmp_a_probe
ACTION="${1:-start}"
URL="${RTMP_URL:-rtmp://push.fast.im/navigation/sn00001_a}"

FPS="${RTMP_FPS:-25}"
BITRATE="${RTMP_BITRATE:-2000000}"
WIDTH="${RTMP_WIDTH:-1920}"
HEIGHT="${RTMP_HEIGHT:-1080}"
CAM="${RTMP_CAM:-0}"
PAUSE_RECOVERY="${RTMP_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"
PID="$ROOT/cam${CAM}_a.pid"
RUN="$ROOT/cam${CAM}_a.run.sh"
LOG="$ROOT/cam${CAM}_a.log"

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[rtmp-a] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[rtmp-a] recovery resumed pid=$pid"
  fi
}

stop_one() {
  if [ -f "$PID" ]; then
    pid="$(cat "$PID" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID"
  fi
  pkill -f "device=/dev/video$CAM" 2>/dev/null || true
  pkill -f "$URL" 2>/dev/null || true
}

status() {
  echo "[rtmp-a] cam$CAM -> $URL"
  echo "[rtmp-a] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[rtmp-a] alive pid=$(cat "$PID") log=$LOG"
  else
    echo "[rtmp-a] stopped log=$LOG"
  fi
  ps -o pid,stat,comm,args | grep recovery | grep -v grep || true
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[rtmp-a] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_rtmp_3push.sh ]; then
      /userdata/start_rtmp_3push.sh stop-all >/dev/null 2>&1 || true
    fi
    if [ -x /userdata/start_rtsp_lan_3.sh ]; then
      /userdata/start_rtsp_lan_3.sh stop-all >/dev/null 2>&1 || true
    fi
    pause_recovery
    stop_one
    : > "$LOG"
    cat > "$RUN" <<EOF
#!/bin/sh
gst-launch-1.0 -q -e \\
  v4l2src device="/dev/video$CAM" io-mode=mmap \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! queue leaky=downstream max-size-buffers=2 \\
  ! mpph264enc bps=$BITRATE gop=$FPS header-mode=1 \\
  ! h264parse config-interval=1 \\
  ! filesink location=/dev/stdout 2>>"$LOG" \\
| ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -re \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f flv -flvflags no_duration_filesize "$URL" >>"$LOG" 2>&1
EOF
    chmod +x "$RUN"
    nohup "$RUN" >/dev/null 2>&1 </dev/null &
    echo $! > "$PID"
    sleep 3
    status
    ;;
  stop|stop-all)
    stop_one
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -80 "$LOG" 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop|stop-all|status|logs]"
    exit 2
    ;;
esac
