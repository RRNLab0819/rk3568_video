# Project Status

Last updated: 2026-05-27

## Completed

| Feature | Status | Notes |
|---------|--------|-------|
| 4-ch V4L2 capture | Done | /dev/video0-3, 1920x1080 NV12, MMAP + dma_buf export |
| 2x2 quad display | Done | Wayland + EGL + GLES2, zero-copy YUV shader from AVM |
| MPP H.265 encode | Done | Per-camera encode, /tmp/cam_%d.h264 |
| RKNN inference | Done | Person-only YOLOv5, RGA dma_buf preprocess option |
| CPU fallback | Done | NV12->RGB via turbojpeg when RGA unavailable |
| fd/memory leak fix | Done | ring_put closes previous fd before overwrite |
| RGA dma_buf test | Done | Isolated test validates importbuffer_fd + wrapbuffer_handle path |
| Fisheye offline verify | Done | Python tool validates calibration + mesh generation |
| Fisheye chessboard calib | Done | 4 cameras, Kannala-Brandt model, RMS <0.4px |
| Fisheye mesh display | Done | 100% UV coverage per camera with tuned FOV |
| AVM layout mode | Done | Sidebar + vehicle placeholder + 4 corrected views |

## In Progress / Partial

| Feature | Status | Notes |
|---------|--------|-------|
| Multi-camera AI | Partial | Round-robin works, needs performance optimization |
| AVM seamless stitching | Not started | Current AVM shows independent tiles, no blending |
| AVM birdview top-down | Not started | Would need multi-camera extrinsics + IPM transform |

## Not Started / Planned

| Feature | Notes |
|---------|-------|
| Detection coordinate mapping | Fish-eye raw coords -> undistorted display coords |
| FreeType text rendering | Current AVM sidebar uses placeholder bars, not real text |
| PNG vehicle model | Current AVM uses GLES primitive vehicle shape |
| Per-camera crop masks | Factory calibinfo has pattern points for ROI |
| Motion detection | Using frame diff or background subtraction |
| RTSP streaming | Encode -> RTSP server |
