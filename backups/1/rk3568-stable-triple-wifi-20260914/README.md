# RK3568 三路无界面录像、WebRTC 与 Wi-Fi 稳定版

来源：2026-09-14 从已验证运行的老板 `49a6d1ec747c9946` 提取。

功能：三路 1920x1080@10fps、每路 1Mbps、中心 80% crop、60 秒 MP4 分段、MediaMTX WebRTC、media-server、SD 卡 90% 到80% 自动清理、网页配网、失败恢复热点、动态同步媒体 IP、联网时禁止后台扫描、保存与恢复正确时间。

默认摄像头为 `/dev/video0`、`/dev/video1`、`/dev/video2`。第三路需要 `/dev/video3` 时，部署后修改 `/userdata/camera_channels.conf` 中的 `HDMI_REC_DEV2=3`。

部署但不复制当前 Wi-Fi 密码：

```sh
./deploy_via_adb.sh
```

在本机存在 `private/wpa_supplicant.conf` 时，复制当前已保存 Wi-Fi：

```sh
./deploy_via_adb.sh --with-saved-wifi
```

`private/` 包含秘密信息，不得提交到 GitHub。

当前内部文件名如 `hdmi_record_switcher` 和 `start_hdmi_record_crop80.sh` 是稳定版兼容路径。先保持原名完成跨板验证，再在独立版本中重构命名。

