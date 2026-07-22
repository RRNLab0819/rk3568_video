#!/bin/sh
set -eu

ROOT=/userdata/rtmp_3push
ACTION="${1:-start}"

URL0="rtmp://push.fast.im/navigation/sn00001_a"
URL1="rtmp://push.fast.im/navigation/sn00001_b"
URL2="rtmp://push.fast.im/navigation/sn00001_c"

FPS="${RTMP_FPS:-8}"
BITRATE="${RTMP_BITRATE:-500000}"
WIDTH="${RTMP_WIDTH:-1280}"
HEIGHT="${RTMP_HEIGHT:-720}"
PAUSE_RECOVERY="${RTMP_PAUSE_RECOVERY:-1}"

mkdir -p "$ROOT"
STATE="$ROOT/state.env"

pid_file() { echo "$ROOT/cam$1.pid"; }
run_file() { echo "$ROOT/cam$1.run.sh"; }
log_file() { echo "$ROOT/cam$1.log"; }

pause_recovery() {
  [ "$PAUSE_RECOVERY" = "1" ] || return
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -STOP "$pid" 2>/dev/null || true
    echo "[rtmp3] recovery paused pid=$pid"
  fi
}

resume_recovery() {
  pid="$(pidof recovery 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill -CONT "$pid" 2>/dev/null || true
    echo "[rtmp3] recovery resumed pid=$pid"
  fi
}

recovery_status() {
  ps -o pid,stat,comm,args | grep recovery | grep -v grep || true
}

stop_cam() {
  cam="$1"
  pf="$(pid_file "$cam")"
  case "$cam" in
    0) url_pat="sn00001_a" ;;
    1) url_pat="sn00001_b" ;;
    2) url_pat="sn00001_c" ;;
    *) url_pat="sn00001" ;;
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
  if [ -f "$pf" ] && kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
    echo "[rtmp3] cam$cam alive pid=$(cat "$pf") log=$lf"
  else
    echo "[rtmp3] cam$cam stopped log=$lf"
  fi
}

status() {
  if [ -f "$STATE" ]; then
    # shellcheck disable=SC1090
    . "$STATE"
  fi
  echo "[rtmp3] fps=$FPS bitrate=$BITRATE size=${WIDTH}x${HEIGHT}"
  echo "[rtmp3] cam0 -> $URL0"
  echo "[rtmp3] cam1 -> $URL1"
  echo "[rtmp3] cam2 -> $URL2"
  status_cam 0
  status_cam 1
  status_cam 2
  recovery_status
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
| ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -re \\
    -f h264 -i pipe:0 -c:v copy -an \\
    -f flv -flvflags no_duration_filesize "$url" >>"$lf" 2>&1
EOF
  chmod +x "$rf"
  nohup "$rf" >/dev/null 2>&1 </dev/null &
  echo $! > "$pf"
}

case "$ACTION" in
  start)
    if pgrep -f rk3568_camera >/dev/null 2>&1; then
      echo "[rtmp3] rk3568_camera is running; stop it before using camera devices"
      exit 1
    fi
    if [ -x /userdata/start_rtsp_lan_3.sh ]; then
      /userdata/start_rtsp_lan_3.sh stop-all >/dev/null 2>&1 || true
    fi
    if [ -x /userdata/start_rtsp_3push.sh ]; then
      /userdata/start_rtsp_3push.sh stop-all >/dev/null 2>&1 || true
    fi
    pause_recovery
    cat > "$STATE" <<EOF
FPS=$FPS
BITRATE=$BITRATE
WIDTH=$WIDTH
HEIGHT=$HEIGHT
PAUSE_RECOVERY=$PAUSE_RECOVERY
EOF
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
    resume_recovery
    status
    ;;
  status)
    status
    ;;
  pause-recovery)
    pause_recovery
    recovery_status
    ;;
  resume-recovery)
    resume_recovery
    recovery_status
    ;;
  logs)
    for cam in 0 1 2; do
      echo "===== cam$cam ====="
      tail -80 "$(log_file "$cam")" 2>/dev/null || true
    done
    ;;
  *)
    echo "usage: $0 [start|stop|stop-all|status|logs|pause-recovery|resume-recovery]"
    exit 2
    ;;
esac
