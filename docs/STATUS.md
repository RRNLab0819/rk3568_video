# Project Status

Last updated: 2026-07-03

## Current Recommended Demo

```bash
cd /userdata
./start_security_rectified.sh
```

Recommended branch:

```text
codex/rectified-direct-stable
```

Stable baseline is still preserved on `codex/stable-4ch-ai-enc-display` and should not be used for experimental fisheye or distance changes.

## Completed

| Feature | Status | Notes |
|---------|--------|-------|
| 4-ch V4L2 capture | Done | `/dev/video0-3`, 1920x1080 NV12, MMAP + dma_buf export |
| 4-ch realtime display | Done | Wayland + EGL + GLES2, security view defaults to four full tiles |
| RKNN YOLOv5 inference | Done | Person-only detection, 4-channel round-robin scheduling |
| Per-camera fisheye calibration load | Done | Reads `/userdata/calib/calib_videoN.yaml` |
| Rectified inference path | Done | RKNN sees rectified 640x640 RGB, so boxes are generated in rectified-image space |
| Detection overlay | Done | Distance labels are compact; duplicate boxes are expected when colocated cameras see the same person |
| Distance demo | Usable prototype | Uses camera height/pitch ground intersection first, falls back to 1.70m person-height estimate |
| MPP H.265 encode | Usable but not default | Per-camera output to `/tmp/cam_N.h264`; higher DDR/MPP/GPU load |
| Stable baseline branch | Done | `codex/stable-4ch-ai-enc-display`, tag `stable-4ch-ai-enc-display-20260527` |

## Prototype / Partial

| Feature | Status | Notes |
|---------|--------|-------|
| Camera extrinsics | Partial | Reads `/userdata/calib/security_extrinsics.ini` or env overrides; values still need real measurement |
| Distance accuracy | Demo only | Good for showing a number, not yet for contractual measurement accuracy |
| AVM/OEM UI | Prototype only | Not current product route; real AVM needs external calibration, IPM/BEV and blending |
| RGA preprocessing | Optional | Code path exists; current rectified direct path is CPU preprocessing + RKNN |
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
2. Use `codex/rectified-direct-stable` for calibrated fisheye, rectified inference and distance demo work.
3. Let RKNN detect on the rectified image directly, instead of detecting on raw fisheye and trying to project boxes afterward.
4. Use camera height and pitch as the next measurement inputs. Until strict extrinsics are measured, distance is approximate.

Current performance target from board tests:

```text
Capture:   ~25 FPS per camera
Display:   ~25 FPS
Inference: ~14 FPS total, about 3.4-3.6 FPS per camera
```
