#!/bin/sh
set -eu

ROOT=/userdata/webrtc_a720
MEDIAMTX="${MEDIAMTX_BIN:-/userdata/rtsp_probe/mediamtx}"
CONF="$ROOT/mediamtx.yml"
ACTION="${1:-start}"

CAM="${WEBRTC_CAM:-0}"
WIDTH="${WEBRTC_WIDTH:-1280}"
HEIGHT="${WEBRTC_HEIGHT:-720}"
FPS="${WEBRTC_FPS:-25}"
BITRATE="${WEBRTC_BITRATE:-1000000}"
PATH_NAME="${WEBRTC_PATH:-cam0}"
PAUSE_RECOVERY="${WEBRTC_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"
PID="$ROOT/${PATH_NAME}.pid"
RUN="$ROOT/${PATH_NAME}.run.sh"
LOG="$ROOT/${PATH_NAME}.log"

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[webrtc-a720] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[webrtc-a720] recovery resumed pid=$pid"
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

stop_stream() {
  kill_pid_file "$PID"
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /rtsp:\/\/127.0.0.1:8554\/cam0|device=\/dev\/video/ {print $1}'); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /rtsp:\/\/127.0.0.1:8554\/cam0|device=\/dev\/video/ {print $1}'); do
    kill -9 "$p" 2>/dev/null || true
  done
}

stop_server() {
  if [ -f "$ROOT/mediamtx.pid" ]; then
    kill "$(cat "$ROOT/mediamtx.pid" 2>/dev/null)" 2>/dev/null || true
    rm -f "$ROOT/mediamtx.pid"
  fi
  pkill -f "$MEDIAMTX $CONF" 2>/dev/null || true
}

start_server() {
  if ! [ -x "$MEDIAMTX" ]; then
    echo "[webrtc-a720] missing $MEDIAMTX"
    exit 1
  fi
  cat > "$CONF" <<'EOF'
logLevel: info
rtsp: true
rtspAddress: :8554
rtmp: false
hls: false
webrtc: true
webrtcAddress: :8889
webrtcAllowOrigins: ['*']
srt: false
api: false
metrics: false
pprof: false
paths:
  all_others:
EOF
  if ! pgrep -f "$MEDIAMTX $CONF" >/dev/null 2>&1; then
    pkill -f "/userdata/rtsp_probe/mediamtx" 2>/dev/null || true
    nohup "$MEDIAMTX" "$CONF" >"$ROOT/mediamtx.log" 2>&1 </dev/null &
    echo $! > "$ROOT/mediamtx.pid"
    sleep 2
  fi
}

board_ip() {
  ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1
}

status() {
  ip="$(board_ip)"
  [ -n "$ip" ] || ip="127.0.0.1"
  echo "[webrtc-a720] cam$CAM -> rtsp://127.0.0.1:8554/$PATH_NAME"
  echo "[webrtc-a720] WHEP: http://$ip:8889/$PATH_NAME/whep"
  echo "[webrtc-a720] viewer: open webrtc_viewer.html?host=$ip&path=$PATH_NAME on PC/VM"
  echo "[webrtc-a720] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[webrtc-a720] alive pid=$(cat "$PID") log=$LOG"
  else
    echo "[webrtc-a720] stopped log=$LOG"
  fi
  ps -o pid,stat,comm,args | grep -E 'mediamtx|gst-launch|ffmpeg|recovery' | grep -v grep || true
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[webrtc-a720] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_rtmp_a720.sh ]; then
      /userdata/start_rtmp_a720.sh stop-all >/dev/null 2>&1 || true
    fi
    if [ -x /userdata/start_rtmp_a.sh ]; then
      /userdata/start_rtmp_a.sh stop-all >/dev/null 2>&1 || true
    fi
    if [ -x /userdata/start_rtmp_3push.sh ]; then
      /userdata/start_rtmp_3push.sh stop-all >/dev/null 2>&1 || true
    fi
    if [ -x /userdata/start_rtsp_lan_3.sh ]; then
      /userdata/start_rtsp_lan_3.sh stop-all >/dev/null 2>&1 || true
    fi
    pause_recovery
    stop_stream
    start_server
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
    -f rtsp -rtsp_transport tcp -muxdelay 0 -muxpreload 0 \\
    "rtsp://127.0.0.1:8554/$PATH_NAME" >>"$LOG" 2>&1
EOF
    chmod +x "$RUN"
    nohup "$RUN" >/dev/null 2>&1 </dev/null &
    echo $! > "$PID"
    sleep 4
    status
    ;;
  stop|stop-all)
    stop_stream
    stop_server
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -80 "$LOG" "$ROOT/mediamtx.log" 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop-all|status|logs]"
    exit 2
    ;;
esac
