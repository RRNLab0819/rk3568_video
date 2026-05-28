# Project Status

Last updated: 2026-05-28

## Completed

| Feature | Status | Notes |
|---------|--------|-------|
| 4-ch V4L2 capture | Done | `/dev/video0-3`, 1920x1080 NV12, MMAP + dma_buf export |
| 2x2 quad display | Done | Wayland + EGL + GLES2, NV12 shader rendering |
| RKNN YOLOv5 inference | Done | Person-only detection, 4-channel round-robin scheduling |
| Detection overlay | Done | Boxes are drawn on grid view and supported AVM main/single views |
| MPP H.265 encode | Usable | Per-camera output to `/tmp/cam_N.h264`; higher load than no-encode mode |
| Startup scripts | Done | `/userdata/start_ai.sh`, `start_ai_enc.sh`, `start_avm.sh`, `start_avm_enc.sh` |
| Stable baseline branch | Done | `codex/stable-4ch-ai-enc-display`, tag `stable-4ch-ai-enc-display-20260527` |

## Prototype / Partial

| Feature | Status | Notes |
|---------|--------|-------|
| OEM AVM UI | Prototype | Layout, view switching, bottom toolbar and assist lines exist, but not production AVM |
| AVM + AI overlay | Partial | Boxes can be shown on main/single camera views; no BEV projection yet |
| Fisheye mesh code | Prototype | Code exists, but current cameras still need formal calibration and validation |
| RGA preprocessing | Optional | Supported by code path, actual best setting depends on board load and stability |
| Encoder long-run stability | Partial | Works in normal tests, still needs longer burn-in under 4-ch AI + display |

## Not Complete

| Feature | Why it matters |
|---------|----------------|
| Formal fisheye calibration | Required for natural-looking correction and any trustworthy geometry |
| Per-camera extrinsics | Required to know each camera's position and angle relative to the vehicle |
| IPM/BEV bird-view mapping | Required for real top-down 360 surround view |
| Seamless stitching/blending | Required to hide boundaries between four cameras |
| Distance estimation | Requires calibrated ground-plane mapping from image coordinates to real-world distance |
| OEM-grade UI assets | Current icons/vehicle are primitives; production UI needs designed bitmap/vector assets |

## Current Recommendation

Use the stable AI/security route for demos that must be reliable:

```bash
/userdata/start_ai.sh
```

Use the AVM route only as a UI and interaction prototype:

```bash
/userdata/start_avm.sh
```

For the next technical milestone, prioritize calibration data collection before spending more time on fake stitching.
