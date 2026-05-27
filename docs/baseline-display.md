# RK3568 Camera — Baseline Display

## Status: STABLE (2026-05-27)

This is the verified baseline. All subsequent AI/RGA/model changes must not break this.

## Capabilities

- 4-channel V4L2 capture from `/dev/video0` through `/dev/video3`
- 1920×1080 @ 25fps NV12 (NV12 = V4L2_PIX_FMT_NV12 = 0x3231564e)
- 2×2 quadrant display via Wayland/EGL/GLES2
- Zero-copy frame passing via dma_buf fd (V4L2 EXPBUF)
- Optional MPP hardware encoding (H.264/H.265) to `/tmp/cam_%d.h264`
- No RKNN, no RGA, no NPU governor, no inference thread, no model loading

## Dependencies (display-only mode)

| Dependency | Required | Notes |
|-----------|----------|-------|
| librga.so | **NO** | Not loaded when inference disabled |
| librknnrt.so | **NO** | Not loaded when inference disabled |
| /userdata/yolov5n_320.rknn | **NO** | Not accessed when inference disabled |
| /sys/class/devfreq/fde40000.npu | **NO** | Not accessed when inference disabled |
| librockchip_mpp.so | YES | Hardware encoder |
| libwayland-client.so | YES | Display |
| libEGL.so, libGLESv2.so | YES | OpenGL ES rendering |
| libdrm.so | YES | dma_buf support |
| /dev/video0~3 | YES | Camera capture |
| Wayland compositor | YES | Display server |

## Run Commands

### Baseline 4-camera display (no AI)
```sh
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera
```

### Single camera display (no AI, no encoding)
```sh
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -c 1 --no-enc
```

### AI single-camera test (opt-in, requires model)
```sh
cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5n_320.rknn -c 1 --no-enc
```

## Scripts

| Script | Purpose | AI Required |
|--------|---------|-------------|
| `start.sh` | Baseline 4-camera display | No |
| `start_display.sh` | Same as start.sh | No |
| `start_ai_cam0.sh` | Single-camera AI person detection | Yes |
| `start_ai.sh` | (legacy) Same as start_ai_cam0.sh | Yes |

## Isolation Design

The AI inference pipeline is gated behind two independent checks:

1. **CLI flag**: `-m <model.rknn>` sets `model[0]` — without this, `model[256] = ""`
2. **Config file**: `[inference] enabled=true` + model path — `ini_get` only reads model when enabled is true

When both are absent:
- `c_RkRgaInit()` is NOT called (gated by `if (model[0] && inf_rga)`)
- NPU governor is NOT checked (gated by `if (model[0])`)
- `pipe_set_inference()` is NOT called (gated by `if (model[0])`)
- Inference thread is NOT spawned (gated by `if (p->inf_cfg)` in pipeline.c)
- `infer_open()` / `rknn_init()` are never reached
- No inference buffers allocated
- Display shows clean video feeds without detection overlay

## Verification Checklist

- [ ] `./rk3568_camera` shows 4-camera 2×2 display
- [ ] No `rknn_init` or `infer_open` in logs
- [ ] Model file absence does not prevent startup
- [ ] `./rk3568_camera -c 1 --no-enc` shows single camera
- [ ] `./rk3568_camera -m /userdata/yolov5n_320.rknn -c 1 --no-enc` enables AI
- [ ] `start_display.sh` works as stable demo script
- [ ] `start_ai_cam0.sh` works as AI test script

## Prohibited Changes

When modifying AI/RGA/inference code, do NOT:
- Make display depend on inference initialization
- Mix display memcpy/import_nv12 with inference preprocessing
- Cause AI errors to affect display startup
- Add unconditional RGA/RKNN/NPU init calls
- Remove the `if (model[0])` guard around inference setup
- Remove the `if (p->inf_cfg)` guard around inference thread creation
