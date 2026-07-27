#!/bin/sh
set -eu

ROOT="${WEBRTC_ROOT:-/userdata/webrtc_single}"
MEDIAMTX="${MEDIAMTX_BIN:-$ROOT/mediamtx}"
CONF="$ROOT/mediamtx.yml"
ACTION="${1:-start}"
STATE="$ROOT/state.env"

CAM="${WEBRTC_CAM:-0}"
PATH_NAME="${WEBRTC_PATH:-cam0}"
WIDTH="${WEBRTC_WIDTH:-1280}"
HEIGHT="${WEBRTC_HEIGHT:-720}"
FPS="${WEBRTC_FPS:-25}"
BITRATE="${WEBRTC_BITRATE:-1500000}"
GOP="${WEBRTC_GOP:-10}"
PAUSE_RECOVERY="${WEBRTC_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"

PID="$ROOT/$PATH_NAME.pid"
RUN="$ROOT/$PATH_NAME.run.sh"
LOG="$ROOT/$PATH_NAME.log"
MTX_LOG="$ROOT/mediamtx.log"

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return 0
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[webrtc] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[webrtc] recovery resumed pid=$pid"
  fi
}

kill_pid_file() {
  pf="$1"
  [ -f "$pf" ] || return 0
  pid="$(cat "$pf" 2>/dev/null || true)"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$pf"
}

stop_camera_streams() {
  kill_pid_file "$PID"
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /device=\/dev\/video|rtsp:\/\/127.0.0.1:8554/ {print $1}'); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(ps -o pid,args | awk '/gst-launch-1.0|ffmpeg/ && /device=\/dev\/video|rtsp:\/\/127.0.0.1:8554/ {print $1}'); do
    kill -9 "$p" 2>/dev/null || true
  done
}

stop_mediamtx() {
  if [ -f "$ROOT/mediamtx.pid" ]; then
    kill "$(cat "$ROOT/mediamtx.pid" 2>/dev/null)" 2>/dev/null || true
    rm -f "$ROOT/mediamtx.pid"
  fi
  pkill -f "$MEDIAMTX $CONF" 2>/dev/null || true
}

start_mediamtx() {
  if ! [ -x "$MEDIAMTX" ]; then
    echo "[webrtc] missing executable: $MEDIAMTX"
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
    pkill -f "/userdata/.*/mediamtx" 2>/dev/null || true
    nohup "$MEDIAMTX" "$CONF" >"$MTX_LOG" 2>&1 </dev/null &
    echo $! > "$ROOT/mediamtx.pid"
    sleep 2
  fi
}

board_ip() {
  ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1
}

load_state() {
  if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE"
  fi
}

write_state() {
  cat > "$STATE" <<EOF
CAM=$CAM
PATH_NAME=$PATH_NAME
WIDTH=$WIDTH
HEIGHT=$HEIGHT
FPS=$FPS
BITRATE=$BITRATE
GOP=$GOP
PAUSE_RECOVERY=$PAUSE_RECOVERY
EOF
}

status() {
  load_state
  ip="$(board_ip)"
  [ -n "$ip" ] || ip="127.0.0.1"
  echo "[webrtc] stream: cam$CAM ${WIDTH}x${HEIGHT}@${FPS} bitrate=$BITRATE gop=$GOP"
  echo "[webrtc] WHEP: http://$ip:8889/$PATH_NAME/whep"
  if [ -f "$PID" ] && kill -0 "$(cat "$PID" 2>/dev/null)" 2>/dev/null; then
    echo "[webrtc] publisher alive pid=$(cat "$PID") log=$LOG"
  else
    echo "[webrtc] publisher stopped log=$LOG"
  fi
  if [ -f "$ROOT/mediamtx.pid" ] && kill -0 "$(cat "$ROOT/mediamtx.pid" 2>/dev/null)" 2>/dev/null; then
    echo "[webrtc] mediamtx alive pid=$(cat "$ROOT/mediamtx.pid") log=$MTX_LOG"
  else
    echo "[webrtc] mediamtx status unknown log=$MTX_LOG"
  fi
  ps -o pid,stat,comm,args | grep -E 'mediamtx|gst-launch|ffmpeg|recovery' | grep -v grep || true
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[webrtc] stopping rk3568_camera because it owns /dev/video*"
      pkill -TERM rk3568_camera 2>/dev/null || true
      sleep 1
      pkill -KILL rk3568_camera 2>/dev/null || true
    fi
    pause_recovery
    stop_camera_streams
    start_mediamtx
    write_state
    : > "$LOG"
    cat > "$RUN" <<EOF
#!/bin/sh
gst-launch-1.0 -q -e \\
  v4l2src device="/dev/video$CAM" io-mode=mmap \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! queue max-size-buffers=8 max-size-time=0 max-size-bytes=0 \\
  ! mpph264enc bps=$BITRATE bps-min=$BITRATE bps-max=$BITRATE rc-mode=cbr profile=baseline gop=$GOP header-mode=1 \\
  ! h264parse config-interval=1 \\
  ! filesink location=/dev/stdout 2>>"$LOG" \\
| ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -use_wallclock_as_timestamps 1 \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f rtsp -rtsp_transport tcp -muxdelay 0 -muxpreload 0 -pkt_size 1200 \\
    "rtsp://127.0.0.1:8554/$PATH_NAME" >>"$LOG" 2>&1
EOF
    chmod +x "$RUN"
    nohup "$RUN" >/dev/null 2>&1 </dev/null &
    echo $! > "$PID"
    sleep 4
    status
    ;;
  stop|stop-all)
    stop_camera_streams
    stop_mediamtx
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -100 "$LOG" "$MTX_LOG" 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop-all|status|logs]"
    exit 2
    ;;
esac
