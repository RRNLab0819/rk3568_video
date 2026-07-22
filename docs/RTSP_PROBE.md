# RTSP Probe

This branch adds an isolated RTSP technical probe. It does not change the stable
4-channel AI/security demo.

## Purpose

- Verify that the RK3568 board can publish one camera over the network.
- Use H.264 for client compatibility.
- Keep load low: one camera, 10 fps, about 0.9 Mbps by default.

## Board Files

- `/userdata/start_rtsp_probe.sh`
- `/userdata/rtsp_probe/mediamtx`
- `/userdata/rtsp_probe/mediamtx.yml`
- `/userdata/rtsp_probe/*.log`

The script needs a MediaMTX ARM64 binary at `/userdata/rtsp_probe/mediamtx`.

Recommended release asset:

```text
mediamtx_v1.19.2_linux_arm64.tar.gz
```

Extract it and copy only the `mediamtx` binary to `/userdata/rtsp_probe/`.

## Start

On the board:

```sh
cd /userdata
./start_rtsp_probe.sh 0 start
```

Open from PC or VM:

```sh
ffplay rtsp://<board-ip>:8554/cam0
```

If the board network is not reachable directly:

```sh
adb forward tcp:8554 tcp:8554
ffplay rtsp://127.0.0.1:8554/cam0
```

## Switch Camera

```sh
./start_rtsp_probe.sh 1 start
./start_rtsp_probe.sh 2 start
./start_rtsp_probe.sh 3 start
```

Only one camera should be tested at a time in this probe.

## Stop

```sh
./start_rtsp_probe.sh 0 stop
./start_rtsp_probe.sh 0 stop-all
```

## Three-Camera Push Probe

This branch also has a separate outbound push script for testing three cameras
against an external RTSP server:

```sh
cd /userdata
./start_rtsp_3push.sh start
./start_rtsp_3push.sh status
./start_rtsp_3push.sh logs
./start_rtsp_3push.sh stop-all
```

Default routes:

```text
cam0 -> rtsp://101.37.23.222:6002/live/hainandaxue/cam01
cam1 -> rtsp://101.37.23.222:6002/live/hainandaxue/cam02
cam2 -> rtsp://101.37.23.222:6002/live/hainandaxue/cam03
```

Defaults are intentionally conservative: 1280x720, 8 fps, 500 kbps per
camera, H.264, GOP 5. Override with environment variables if needed:

```sh
RTSP_WIDTH=1920 RTSP_HEIGHT=1080 RTSP_FPS=10 RTSP_BITRATE=800000 ./start_rtsp_3push.sh start
```

The board image currently starts `/usr/bin/recovery`, which can consume a full
CPU core. The three-camera script pauses that process with `SIGSTOP` while
streaming and resumes it on `stop-all`. This does not modify system files.

```sh
./start_rtsp_3push.sh pause-recovery
./start_rtsp_3push.sh resume-recovery
```

Disable the automatic pause if needed:

```sh
RTSP_PAUSE_RECOVERY=0 ./start_rtsp_3push.sh start
```

## LAN Three-Camera Probe

Use this to remove the cloud RTSP server from the latency test:

```sh
cd /userdata
./start_rtsp_lan_3.sh start
./start_rtsp_lan_3.sh status
./start_rtsp_lan_3.sh stop-all
```

Default LAN URLs:

```text
rtsp://192.168.2.54:8554/cam0
rtsp://192.168.2.54:8554/cam1
rtsp://192.168.2.54:8554/cam2
```

The LAN probe uses the same conservative defaults as the cloud push probe:
1280x720, 8 fps, 500 kbps per camera.

## RTMP Three-Camera Push Probe

Use this script to push three raw camera streams to the RTMP service:

```sh
cd /userdata
./start_rtmp_3push.sh start
./start_rtmp_3push.sh status
./start_rtmp_3push.sh logs
./start_rtmp_3push.sh stop-all
```

Default routes:

```text
cam0 -> rtmp://push.fast.im/navigation/sn00001_a
cam1 -> rtmp://push.fast.im/navigation/sn00001_b
cam2 -> rtmp://push.fast.im/navigation/sn00001_c
```

Default parameters are 1280x720, 8 fps, 500 kbps per camera. Override them
without editing the script:

```sh
RTMP_WIDTH=1920 RTMP_HEIGHT=1080 RTMP_FPS=10 RTMP_BITRATE=800000 ./start_rtmp_3push.sh start
```

## RTMP Single A Probe

Use this for a higher-quality single-stream test to the `_a` route:

```sh
cd /userdata
./start_rtmp_a.sh start
./start_rtmp_a.sh status
./start_rtmp_a.sh logs
./start_rtmp_a.sh stop-all
```

Default route and parameters:

```text
cam0 -> rtmp://push.fast.im/navigation/sn00001_a
1920x1080, 25 fps, 2 Mbps
```

Override without editing the script:

```sh
RTMP_CAM=1 RTMP_FPS=20 RTMP_BITRATE=1500000 ./start_rtmp_a.sh start
```

## Notes

- The stable AI program must be stopped before running this probe because both
  need the same `/dev/videoN` device.
- The capture/encode path is `GStreamer v4l2src -> Rockchip mpph264enc`.
- `ffmpeg` only forwards the H.264 stream into MediaMTX as RTSP; it does not
  encode the video.
