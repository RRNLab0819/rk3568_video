# TODO

Priority: P0 = critical, P1 = important, P2 = nice-to-have.

## P0 - Calibration and Truthful AVM Foundation

- [ ] Collect formal checkerboard images on the RK3568 board for all four cameras.
- [ ] Run OpenCV fisheye calibration per camera and record intrinsics/distortion.
- [ ] Validate GLES fisheye undistort mesh with live video, not only offline samples.
- [ ] Define camera mounting order, rotation, flip and vehicle-relative orientation.
- [ ] Add a checked-in calibration config format for per-camera parameters.

## P1 - Real Surround View

- [ ] Estimate per-camera extrinsics relative to vehicle/body frame.
- [ ] Implement IPM/BEV ground-plane mapping for front/rear/left/right cameras.
- [ ] Add blend masks to reduce visible seams between camera regions.
- [ ] Replace primitive vehicle drawing with a cleaner vehicle texture/model.
- [ ] Improve bottom toolbar icons and selected-state styling.
- [ ] Keep current 2x2 grid path unchanged as stable fallback.

## P1 - AI and Distance

- [ ] Map detection boxes through calibrated undistort/IPM geometry.
- [ ] Use person foot-point on ground plane to estimate distance.
- [ ] Add configurable warning zones/guide lines for front and rear views.
- [ ] Run long-duration test for 4-ch AI + display + optional encoder.

## P2 - Product Enhancements

- [ ] RTSP/HLS streaming from encoded output.
- [ ] Motion detection and event-triggered recording.
- [ ] OSD timestamp/text overlay per channel.
- [ ] Performance profiling: GPU, NPU, DDR bandwidth, MPP encoder.
- [ ] External UI assets for OEM-style toolbar and vehicle overlay.

## Done

- [x] Four-channel V4L2 capture.
- [x] 2x2 Wayland/EGL/GLES2 display.
- [x] RKNN YOLOv5 person detection.
- [x] Detection boxes in stable display path.
- [x] Optional MPP H.265 encoding.
- [x] Startup scripts for AI and AVM prototype.
- [x] Project technical summary document.
