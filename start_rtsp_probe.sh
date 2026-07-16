#!/bin/sh
set -eu

ROOT=/userdata/rtsp_probe
MEDIAMTX="$ROOT/mediamtx"
CONF="$ROOT/mediamtx.yml"
CAM="${1:-0}"
ACTION="${2:-start}"

case "$CAM" in
  cam0) CAM=0 ;;
  cam1) CAM=1 ;;
  cam2) CAM=2 ;;
  cam3) CAM=3 ;;
esac

if [ "$CAM" != "0" ] && [ "$CAM" != "1" ] && [ "$CAM" != "2" ] && [ "$CAM" != "3" ]; then
  echo "usage: $0 [0|1|2|3|cam0|cam1|cam2|cam3] [start|stop|status]"
  exit 2
fi

mkdir -p "$ROOT"

server_pid="$ROOT/mediamtx.pid"
pub_pid="$ROOT/cam${CAM}.publisher.pid"
pub_log="$ROOT/cam${CAM}.publisher.log"
pub_run="$ROOT/cam${CAM}.publisher.sh"
server_log="$ROOT/mediamtx.log"

stop_cam() {
  if [ -f "$pub_pid" ]; then
    pid="$(cat "$pub_pid" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pub_pid"
  fi
}

stop_all() {
  for f in "$ROOT"/cam*.publisher.pid; do
    [ -f "$f" ] || continue
    pid="$(cat "$f" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$f"
  done
  if [ -f "$server_pid" ]; then
    pid="$(cat "$server_pid" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$server_pid"
  fi
}

status() {
  echo "[rtsp] root=$ROOT"
  if [ -x "$MEDIAMTX" ]; then
    echo "[rtsp] mediamtx=ok"
  else
    echo "[rtsp] mediamtx=missing ($MEDIAMTX)"
  fi
  for f in "$server_pid" "$ROOT"/cam*.publisher.pid; do
    [ -f "$f" ] || continue
    pid="$(cat "$f" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      echo "[rtsp] alive $(basename "$f") pid=$pid"
    else
      echo "[rtsp] dead  $(basename "$f") pid=$pid"
    fi
  done
  ip="$(ip addr show wlan0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)"
  [ -n "$ip" ] || ip="$(ip addr show eth0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)"
  [ -n "$ip" ] && echo "[rtsp] url=rtsp://$ip:8554/cam$CAM"
  echo "[rtsp] logs: $server_log $pub_log"
}

case "$ACTION" in
  stop)
    stop_cam
    status
    exit 0
    ;;
  stop-all)
    stop_all
    status
    exit 0
    ;;
  status)
    status
    exit 0
    ;;
  start) ;;
  *)
    echo "usage: $0 [0|1|2|3|cam0|cam1|cam2|cam3] [start|stop|stop-all|status]"
    exit 2
    ;;
esac

if ! [ -x "$MEDIAMTX" ]; then
  echo "[rtsp] missing MediaMTX binary: $MEDIAMTX"
  echo "[rtsp] put the ARM64 release binary there, for example from:"
  echo "[rtsp]   mediamtx_v1.19.2_linux_arm64.tar.gz"
  echo "[rtsp] then chmod +x $MEDIAMTX"
  exit 1
fi

if pgrep -f rk3568_camera >/dev/null 2>&1; then
  echo "[rtsp] rk3568_camera is already running; stop it before this probe uses /dev/video$CAM"
  exit 1
fi

cat > "$CONF" <<'EOF'
logLevel: info
rtsp: true
rtspAddress: :8554
rtmp: false
hls: false
webrtc: false
srt: false
api: false
metrics: false
pprof: false
paths:
  all_others:
EOF

if ! [ -f "$server_pid" ] || ! kill -0 "$(cat "$server_pid" 2>/dev/null)" 2>/dev/null; then
  : > "$server_log"
  nohup "$MEDIAMTX" "$CONF" >"$server_log" 2>&1 </dev/null &
  echo $! > "$server_pid"
  sleep 1
fi

stop_cam
: > "$pub_log"

FPS="${RTSP_FPS:-10}"
BITRATE="${RTSP_BITRATE:-900000}"
WIDTH=1920
HEIGHT=1080

cat > "$pub_run" <<EOF
#!/bin/sh
gst-launch-1.0 -q -e \\
  v4l2src device="/dev/video$CAM" io-mode=mmap \\
  ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 \\
  ! queue leaky=downstream max-size-buffers=2 \\
  ! mpph264enc bps=$BITRATE gop=$FPS header-mode=1 \\
  ! h264parse config-interval=1 \\
  ! filesink location=/dev/stdout 2>>"$pub_log" \\
| ffmpeg -hide_banner -loglevel warning -fflags nobuffer -re \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f rtsp -rtsp_transport tcp "rtsp://127.0.0.1:8554/cam$CAM" >>"$pub_log" 2>&1
EOF
chmod +x "$pub_run"

nohup "$pub_run" >/dev/null 2>&1 </dev/null &
echo $! > "$pub_pid"

sleep 2
status
echo "[rtsp] from PC/VM: ffplay rtsp://<board-ip>:8554/cam$CAM"
echo "[rtsp] adb fallback: adb forward tcp:8554 tcp:8554; ffplay rtsp://127.0.0.1:8554/cam$CAM"
