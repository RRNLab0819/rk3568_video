# Complete HDMI + SD Record + WebRTC Workflow

This branch combines three functions in one camera pipeline:

```text
cam0/cam1 V4L2 -> center 80% crop -> HDMI switch display
                              -> one H.264 encoder per camera
                              -> MP4 segments on SD card
                              -> WebRTC preview through MediaMTX
```

The important point is that `/dev/video0` and `/dev/video1` are opened only
once. WebRTC does not start a second capture process, so it does not fight the
local HDMI/SD recording workflow.

## Start

```sh
cd /userdata
./start_complete_work.sh start
```

Default output:

```text
HDMI: keyboard 1=cam0, 2=cam1, 3=latest file paths
SD cam0: /mnt/sdcard/rk3568_recordings/cam0/YYYY-MM-DD/HH-MM-SS_HH-MM-SS.mp4
SD cam1: /mnt/sdcard/rk3568_recordings/cam1/YYYY-MM-DD/HH-MM-SS_HH-MM-SS.mp4
WebRTC viewer cam0: http://<board-ip>:8889/cam0/
WebRTC viewer cam1: http://<board-ip>:8889/cam1/
WHEP API cam0: http://<board-ip>:8889/cam0/whep
WHEP API cam1: http://<board-ip>:8889/cam1/whep
```

The current default is 1920x1080 at 15 fps, 4 Mbps per camera, center 80% crop,
and 60-second MP4 segments. This keeps the RK3568 load lower than 1080p25 while
preserving a usable local display and network preview.

## Stop

```sh
./start_complete_work.sh stop
```

Always stop cleanly before removing the SD card. MP4 metadata is finalized when
a segment closes or when the recorder receives the stop signal.

## Status And Logs

```sh
./start_complete_work.sh status
./start_complete_work.sh logs
```

## Tune

Try 25 fps:

```sh
HDMI_REC_FPS=25 HDMI_REC_GOP=25 ./start_complete_work.sh start
```

Lower network/storage bitrate:

```sh
HDMI_REC_BITRATE=2500000 ./start_complete_work.sh start
```

Keep more fisheye image:

```sh
HDMI_REC_CROP_PERCENT=90 ./start_complete_work.sh start
```

Use a different MediaMTX binary:

```sh
MEDIAMTX_BIN=/userdata/webrtc_single/mediamtx ./start_complete_work.sh start
```

## Components

- `tools/hdmi_record_switcher.c`: one-process dual-camera capture, HDMI switch,
  MP4 splitting, and optional WebRTC pipe output.
- `start_hdmi_record_crop80.sh`: stable HDMI + SD recorder control script.
- `start_complete_work.sh`: starts MediaMTX and then starts the recorder with
  `HDMI_REC_WEBRTC=1`.
- `webrtc_single_viewer.html`: browser helper page for WHEP playback.

## Notes

WebRTC preview receives the same cropped H.264 stream that is recorded. It is
intended for local-network viewing and product demos, not cloud relay.
