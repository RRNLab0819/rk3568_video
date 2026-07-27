# Single-Camera WebRTC Probe

This is the current clean LAN preview path. It replaces the older RTMP, RTSP,
and three-stream probe scripts for daily testing.

## Board Start

```sh
cd /userdata
./start_webrtc_single.sh start
```

Default stream:

```text
cam0, 1920x1080, 15 fps, H.264, 2 Mbps
center 80% crop, scaled back to 1920x1080
```

## Tune Without Editing

```sh
WEBRTC_CAM=1 WEBRTC_PATH=cam1 ./start_webrtc_single.sh start
WEBRTC_CROP_PERCENT=90 ./start_webrtc_single.sh start
WEBRTC_WIDTH=1280 WEBRTC_HEIGHT=720 WEBRTC_FPS=25 WEBRTC_BITRATE=1500000 WEBRTC_CROP_PERCENT=100 ./start_webrtc_single.sh start
WEBRTC_BITRATE=2500000 ./start_webrtc_single.sh start
```

## Status And Logs

```sh
./start_webrtc_single.sh status
./start_webrtc_single.sh logs
./start_webrtc_single.sh stop-all
```

## Browser

Open the viewer from the VM or any computer on the same LAN:

```text
http://<vm-ip>:8090/webrtc_single_viewer.html?host=<board-ip>&path=cam0
```

During current testing:

```text
board-ip = 192.168.2.54
```

The viewer connects to:

```text
http://<board-ip>:8889/cam0/whep
```
