# HDMI Crop80 SD Recording

This probe is independent from the stable AI display programs. It opens one
camera, shows the same processed image on HDMI, and records it to the SD card.

Default:

```text
cam0
1920x1080
15 fps by default
center 80% crop scaled back to 1920x1080
H.264 MP4 segments
60 seconds per file
record directory: /mnt/sdcard/rk3568_recordings
```

## Start

```sh
cd /userdata
./start_hdmi_record_crop80.sh start
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
HDMI_REC_CAM=1 ./start_hdmi_record_crop80.sh start
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
