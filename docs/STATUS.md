# Project Status

Last updated: 2026-07-03

## Current Recommended Demo

Run on the RK3568 board:

```bash
cd /userdata
./start_security_fisheye_height.sh
```

Recommended branch:

```text
codex/security-fisheye-height-crop80
```

This branch is the currently accepted demo version: raw fisheye 2x2 display, center crop 80%, raw-coordinate YOLO boxes, and 1.70m person-height distance labels.

Stable baseline is still preserved on `codex/stable-4ch-ai-enc-display` with tag `stable-4ch-ai-enc-display-20260527`. Do not use the stable baseline for experimental fisheye, UI, or distance changes.

## Completed

| Feature | Status | Notes |
|---------|--------|-------|
| 4-ch V4L2 capture | Done | `/dev/video0-3`, 1920x1080 NV12, MMAP + dma_buf export |
| Stable frame copy | Done | Display/inference/encoder use packed NV12 copies to avoid V4L2 buffer reuse issues |
| 4-ch realtime display | Done | Wayland + EGL + GLES2, current demo uses 2x2 raw fisheye grid |
| RKNN YOLOv5 inference | Done | Person-only detection, 4-channel round-robin scheduling |
| Per-camera calibration load | Done | Reads `/userdata/calib/calib_videoN.yaml` for intrinsics/focal data |
| Raw-coordinate detection overlay | Done | Current recommended path avoids rectified projection drift |
| Center-crop display mapping | Done | `SECURITY_RAW_CROP=0.80`, detection boxes are mapped with the same crop |
| Distance demo | Usable prototype | Uses 1.70m person-height estimate in the current recommended demo |
| MPP H.265 encode | Usable but not default | Per-camera output to `/tmp/cam_N.h264`; higher DDR/MPP/GPU load |
| Stable baseline branch | Done | `codex/stable-4ch-ai-enc-display`, tag `stable-4ch-ai-enc-display-20260527` |

## Prototype / Partial

| Feature | Status | Notes |
|---------|--------|-------|
| Rectified direct inference | Experimental | Branch `codex/rectified-direct-stable`; boxes align well but inference update rate is lower |
| Camera extrinsics | Partial | Reads `/userdata/calib/security_extrinsics.ini` or env overrides; values still need real measurement |
| Distance accuracy | Demo only | Good for showing a rough number, not yet for contractual measurement accuracy |
| AVM/OEM UI | Prototype only | Not current product route; real AVM needs external calibration, IPM/BEV and blending |
| RGA preprocessing | Optional | Code paths exist; current recommended demo uses CPU preprocessing + RKNN |
| Encoder long-run stability | Partial | Display + AI is preferred for demos; encode adds system pressure |

## Not Complete

| Feature | Why it matters |
|---------|----------------|
| Formal extrinsic calibration | Required for trustworthy ground-plane distance and camera-to-camera geometry |
| Real distance validation | Need measured samples at known distances for every camera |
| IPM/BEV bird-view mapping | Required for true surround-view stitching |
| Seamless stitching/blending | Required for OEM AVM appearance |
| UI asset polish | Security demo is functional; final product UI still needs designed assets and layout tuning |
| `display.c` split | Current file mixes base display, old AVM, security OSD and helper math |

## Current Technical Position

The best current route is security-oriented, not OEM AVM-oriented:

1. Keep the stable 4-channel baseline untouched.
2. Use `codex/security-fisheye-height-crop80` for the current product demo.
3. Keep display and inference in raw fisheye coordinates so boxes stay aligned.
4. Use center crop to reduce the strongest fisheye edge distortion without changing inference.
5. Treat 1.70m person-height distance as a demo estimate only.
6. Move to strict extrinsics and ground-plane intersection when real measurement accuracy is required.

Current performance from board tests:

```text
Capture:   ~25 FPS per camera
Display:   ~25 FPS
Inference: ~16.4-16.6 FPS total, about 4.0-4.2 FPS per camera
```
