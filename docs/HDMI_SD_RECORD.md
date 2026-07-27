# HDMI Crop80 SD Recording

This probe is independent from the stable AI display programs. It opens one
camera, shows the same processed image on HDMI, and records it to the SD card.

Default:

```text
cam0 and cam1 when `tools/hdmi_record_switcher` is deployed
1920x1080
15 fps by default
center 80% crop scaled back to 1920x1080
H.264 MP4 segments
60 seconds per file
record directories:

```text
/mnt/sdcard/rk3568_recordings/cam0/YYYY-MM-DD/HH-MM-SS_00000.mp4
/mnt/sdcard/rk3568_recordings/cam1/YYYY-MM-DD/HH-MM-SS_00000.mp4
```
```

## Start

```sh
cd /userdata
./start_hdmi_record_crop80.sh start
```

Keyboard while running:

```text
1 = show cam0 on HDMI
2 = show cam1 on HDMI
3 = print latest recording paths in the log
q = stop
```

If a USB keyboard is present, the script tries to detect its `/dev/input/eventX`
automatically. To force a device:

```sh
HDMI_REC_EVENT=/dev/input/event3 ./start_hdmi_record_crop80.sh start
```

To test switching from the ADB terminal, run in foreground and press `1`, `2`,
or `3` in that terminal:

```sh
HDMI_REC_FOREGROUND=1 ./start_hdmi_record_crop80.sh start
```

## Stop

```sh
./start_hdmi_record_crop80.sh stop
```

## Status And Logs

```sh
./start_hdmi_record_crop80.sh status
./start_hdmi_record_crop80.sh logs
```

## Tune Without Editing

Use 15 fps:

```sh
HDMI_REC_FPS=15 HDMI_REC_GOP=15 ./start_hdmi_record_crop80.sh start
```

Try 25 fps:

```sh
HDMI_REC_FPS=25 HDMI_REC_GOP=25 ./start_hdmi_record_crop80.sh start
```

Keep more fisheye image:

```sh
HDMI_REC_CROP_PERCENT=90 ./start_hdmi_record_crop80.sh start
```

Use another camera:

```sh
HDMI_REC_DUAL=0 HDMI_REC_CAM=1 ./start_hdmi_record_crop80.sh start
```

Change bitrate:

```sh
HDMI_REC_BITRATE=2500000 ./start_hdmi_record_crop80.sh start
```

Set an explicit output directory:

```sh
HDMI_REC_DIR=/mnt/sdcard/demo_records ./start_hdmi_record_crop80.sh start
```

When no explicit directory is set, the script only writes to the mounted SD
card at `/mnt/sdcard`. This avoids accidentally filling the small internal
`/userdata` partition.

## Notes

This is not full calibrated fisheye undistortion. It uses a low-risk center
crop and scale path so HDMI display and the recorded file match while keeping
RK3568 load controlled.
