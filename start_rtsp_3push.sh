#!/bin/sh
set -eu

ROOT=/userdata/rtsp_3push
ACTION="${1:-start}"

URL0="rtsp://101.37.23.222:6002/live/hainandaxue/cam01"
URL1="rtsp://101.37.23.222:6002/live/hainandaxue/cam02"
URL2="rtsp://101.37.23.222:6002/live/hainandaxue/cam03"

FPS="${RTSP_FPS:-10}"
BITRATE="${RTSP_BITRATE:-800000}"
WIDTH="${RTSP_WIDTH:-1920}"
HEIGHT="${RTSP_HEIGHT:-1080}"

mkdir -p "$ROOT"

pid_file() {
  echo "$ROOT/cam$1.pid"
}

run_file() {
  echo "$ROOT/cam$1.run.sh"
}

log_file() {
  echo "$ROOT/cam$1.log"
}

stop_cam() {
  cam="$1"
  pf="$(pid_file "$cam")"
  case "$cam" in
    0) url_pat="hainandaxue/cam01" ;;
    1) url_pat="hainandaxue/cam02" ;;
    2) url_pat="hainandaxue/cam03" ;;
    *) url_pat="hainandaxue/cam" ;;
  esac
  if [ -f "$pf" ]; then
    pid="$(cat "$pf" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pf"
  fi
  pkill -f "device=/dev/video$cam" 2>/dev/null || true
  pkill -f "$url_pat" 2>/dev/null || true
}

stop_all() {
  stop_cam 0
  stop_cam 1
  stop_cam 2
}

status_cam() {
  cam="$1"
  pf="$(pid_file "$cam")"
  lf="$(log_file "$cam")"
  if [ -f "$pf" ]; then
    pid="$(cat "$pf" 2>/dev/null || true)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      echo "[3push] cam$cam alive pid=$pid log=$lf"
    else
      echo "[3push] cam$cam dead pid=$pid log=$lf"
    fi
  else
    echo "[3push] cam$cam stopped log=$lf"
  fi
}

status() {
  echo "[3push] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  echo "[3push] cam0 -> $URL0"
  echo "[3push] cam1 -> $URL1"
  echo "[3push] cam2 -> $URL2"
  status_cam 0
  status_cam 1
  status_cam 2
}

start_cam() {
  cam="$1"
  url="$2"
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
  ! mpph264enc bps=$BITRATE gop=5 header-mode=1 \\
  ! h264parse config-interval=1 \\
  ! filesink location=/dev/stdout 2>>"$lf" \\
| ffmpeg -hide_banner -loglevel warning \\
    -fflags nobuffer -flags low_delay -re \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f rtsp -rtsp_transport tcp -muxdelay 0 -muxpreload 0 "$url" >>"$lf" 2>&1
EOF
  chmod +x "$rf"
  nohup "$rf" >/dev/null 2>&1 </dev/null &
  echo $! > "$pf"
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[3push] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_rtsp_probe.sh ]; then
      /userdata/start_rtsp_probe.sh 0 stop-all >/dev/null 2>&1 || true
    fi
    start_cam 0 "$URL0"
    sleep 1
    start_cam 1 "$URL1"
    sleep 1
    start_cam 2 "$URL2"
    sleep 3
    status
    ;;
  stop|stop-all)
    stop_all
    status
    ;;
  status)
    status
    ;;
  logs)
    for cam in 0 1 2; do
      echo "===== cam$cam ====="
      tail -80 "$(log_file "$cam")" 2>/dev/null || true
    done
    ;;
  *)
    echo "usage: $0 [start|stop|stop-all|status|logs]"
    exit 2
    ;;
esac
