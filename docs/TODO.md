# TODO

Priority: P0 = critical, P1 = important, P2 = nice-to-have

## P0 — Stability & Correctness

- [ ] Fix ADB disconnection when running Wayland display tests (GPU/Mali interaction)
- [ ] Add `killall` safety before startup to prevent "device busy"
- [ ] Verify fisheye mesh coverage with live video, not just offline scan
- [ ] Test all 3 display modes (grid, fisheye, AVM) under continuous 24h run

## P1 — Feature Completion

- [ ] Detection box coordinate mapping: fisheye raw coords -> corrected display coords
- [ ] AVM vehicle placeholder: replace GLES primitive with PNG texture
- [ ] AVM sidebar: real text via bitmap font or FreeType
- [ ] Per-camera FOV auto-detection from calibration (replace hardcoded defaults)
- [ ] External config file for per-camera fisheye params (cx, cy, focal, k1-4, FOV)
- [ ] Multi-camera AI: optimize round-robin scheduling for 4-ch real-time

## P2 — Enhancement

- [ ] AVM seamless stitching: multi-camera extrinsics + blend zone
- [ ] AVM birdview: inverse perspective mapping + ground plane assumption
- [ ] RTSP/HLS streaming from encoder output
- [ ] Motion detection and event-triggered recording
- [ ] OSD timestamp/text overlay per channel
- [ ] Performance profiling: GPU utilization, NPU throughput, memory bandwidth
- [ ] Remove unused `lens_6028_table` from fisheye_mesh.c (now replaced by Kannala-Brandt)

## Done (historical)

- [x] 4-ch capture + 2x2 display
- [x] MPP H.265 encoding
- [x] RKNN YOLOv5 inference + person detection
- [x] RGA dma_buf preprocessing
- [x] fd/memory leak fix in ring_put
- [x] Chessboard fisheye calibration (4 cameras)
- [x] Fisheye GPU mesh undistort (Kannala-Brandt model)
- [x] AVM layout mode (sidebar + vehicle + 4 views)
- [x] Project cleanup and documentation
