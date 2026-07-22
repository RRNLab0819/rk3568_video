#!/bin/sh
set -eu

ROOT=/userdata/rtmp_a720_push
ACTION="${1:-start}"

CAM="${RTMP_CAM:-0}"
URL="${RTMP_URL:-rtmp://push.fast.im/navigation/sn00001_a}"
WIDTH="${RTMP_WIDTH:-1280}"
HEIGHT="${RTMP_HEIGHT:-720}"
FPS="${RTMP_FPS:-25}"
BITRATE="${RTMP_BITRATE:-1000000}"
PAUSE_RECOVERY="${RTMP_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"
PID="$ROOT/cam${CAM}.pid"
RUN="$ROOT/cam${CAM}.run.sh"
LOG="$ROOT/cam${CAM}.log"

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[rtmp-a720] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[rtmp-a720] recovery resumed pid=$pid"
  fi
}

kill_pid_file() {
  pf="$1"
  if ! [ -f "$pf" ]; then
    return 0
  fi
  pid="$(cat "$pf" 2>/dev/null || true)"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$pf"
}

stop_known_streams() {
  kill_pid_file "$PID"
  for pf in /userdata/rtmp_ab_push/*.pid /userdata/rtmp_3push/*.pid /userdata/rtmp_a_probe/*.pid; do
    [ -f "$pf" ] && kill_pid_file "$pf"
  done
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /sn00001_|device=\/dev\/video/ {print $1}'); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /sn00001_|device=\/dev\/video/ {print $1}'); do
    kill -9 "$p" 2>/dev/null || true
  done
}

status() {
  echo "[rtmp-a720] cam$CAM -> $URL"
  echo "[rtmp-a720] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[rtmp-a720] alive pid=$(cat "$PID") log=$LOG"
  else
    echo "[rtmp-a720] stopped log=$LOG"
  fi
  ps -o pid,stat,comm,args | grep -E 'gst-launch|ffmpeg|recovery' | grep -v grep || true
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[rtmp-a720] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_webrtc_a720.sh ]; then
      /userdata/start_webrtc_a720.sh stop-all >/dev/null 2>&1 || true
    fi
    pause_recovery
    stop_known_streams
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
    sleep 4
    status
    ;;
  stop|stop-all)
    stop_known_streams
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
    echo "usage: $0 [start|stop-all|status|logs]"
    exit 2
    ;;
esac
