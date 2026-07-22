#!/bin/sh
set -eu

ROOT=/userdata/webrtc_3x720
MEDIAMTX="${MEDIAMTX_BIN:-/userdata/rtsp_probe/mediamtx}"
CONF="$ROOT/mediamtx.yml"
ACTION="${1:-start}"
STATE="$ROOT/state.env"

WIDTH="${WEBRTC_WIDTH:-1280}"
HEIGHT="${WEBRTC_HEIGHT:-720}"
FPS="${WEBRTC_FPS:-20}"
BITRATE="${WEBRTC_BITRATE:-800000}"
PAUSE_RECOVERY="${WEBRTC_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"

pid_file() { echo "$ROOT/cam$1.pid"; }
run_file() { echo "$ROOT/cam$1.run.sh"; }
log_file() { echo "$ROOT/cam$1.log"; }

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[webrtc3] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[webrtc3] recovery resumed pid=$pid"
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

stop_cam() {
  cam="$1"
  kill_pid_file "$(pid_file "$cam")"
  for p in $(ps -o pid,args | awk -v cam="$cam" '/gst-launch-1.0|ffmpeg/ && ($0 ~ "device=/dev/video" cam || $0 ~ "rtsp://127.0.0.1:8554/cam" cam) {print $1}'); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(ps -o pid,args | awk -v cam="$cam" '/gst-launch-1.0|ffmpeg/ && ($0 ~ "device=/dev/video" cam || $0 ~ "rtsp://127.0.0.1:8554/cam" cam) {print $1}'); do
    kill -9 "$p" 2>/dev/null || true
  done
}

stop_all_streams() {
  stop_cam 0
  stop_cam 1
  stop_cam 2
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
    echo "[webrtc3] missing $MEDIAMTX"
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

start_cam() {
  cam="$1"
  pf="$(pid_file "$cam")"
  rf="$(run_file "$cam")"
  lf="$(log_file "$cam")"
  stop_cam "$cam"
  : > "$lf"
  cat > "$rf" <<EOF
#!/bin/sh
gst-launch-1.0 -q -e \\
  v4l2src device="/dev/video$cam" io-mode=mmap \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! queue leaky=downstream max-size-buffers=2 \\
  ! mpph264enc bps=$BITRATE gop=$FPS header-mode=1 \\
  ! h264parse config-interval=1 \\
  ! filesink location=/dev/stdout 2>>"$lf" \\
| ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -re \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f rtsp -rtsp_transport tcp -muxdelay 0 -muxpreload 0 \\
    "rtsp://127.0.0.1:8554/cam$cam" >>"$lf" 2>&1
EOF
  chmod +x "$rf"
  nohup "$rf" >/dev/null 2>&1 </dev/null &
  echo $! > "$pf"
}

board_ip() {
  ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1
}

status() {
  if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE"
  fi
  ip="$(board_ip)"
  [ -n "$ip" ] || ip="127.0.0.1"
  echo "[webrtc3] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  for cam in 0 1 2; do
    echo "[webrtc3] cam$cam WHEP: http://$ip:8889/cam$cam/whep"
    pf="$(pid_file "$cam")"
    if [ -f "$pf" ] && kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
      echo "[webrtc3] cam$cam alive pid=$(cat "$pf") log=$(log_file "$cam")"
    else
      echo "[webrtc3] cam$cam stopped log=$(log_file "$cam")"
    fi
  done
  ps -o pid,stat,comm,args | grep -E 'mediamtx|gst-launch|ffmpeg|recovery' | grep -v grep || true
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[webrtc3] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_webrtc_a720.sh ]; then
      /userdata/start_webrtc_a720.sh stop-all >/dev/null 2>&1 || true
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
    pause_recovery
    start_server
    cat > "$STATE" <<EOF
WIDTH=$WIDTH
HEIGHT=$HEIGHT
FPS=$FPS
BITRATE=$BITRATE
PAUSE_RECOVERY=$PAUSE_RECOVERY
EOF
    start_cam 0
    sleep 1
    start_cam 1
    sleep 1
    start_cam 2
    sleep 4
    status
    ;;
  stop|stop-all)
    stop_all_streams
    stop_server
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  logs)
    tail -80 "$ROOT"/cam*.log "$ROOT"/mediamtx.log 2>/dev/null || true
    ;;
  *)
    echo "usage: $0 [start|stop-all|status|logs]"
    exit 2
    ;;
esac
