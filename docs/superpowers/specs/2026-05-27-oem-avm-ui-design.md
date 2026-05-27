# OEM AVM UI Design

Date: 2026-05-27
Base stable version: 233f0e1 stabilize display texture updates
Protected stable tag: stable-4ch-ai-enc-display-20260527

## Goal

Build a new OEM-style AVM display mode for the RK3568 4-channel camera project. The current stable 2x2 AI display and 2x2 AI+encoder flows must remain unchanged and recoverable.

The first AVM version should feel like the reference factory UI: a black toolbar, white mode icons, a red active selection block, switchable camera layouts, and a car-centered surround-view presentation. It does not need to be true calibrated 3D AVM in the first pass.

## Non-Goals For First Version

- Do not change the default 2x2 display behavior.
- Do not change /userdata/start_ai.sh or /userdata/start_ai_enc.sh behavior.
- Do not depend on touch input.
- Do not require external image assets for icons or vehicle graphics.
- Do not implement true seamless bird's-eye stitching, camera extrinsic calibration, or physical vehicle geometry projection yet.
- Do not keep the old experimental AVM code path; replace it with a new isolated OEM AVM mode.

## Entry Points

Existing scripts stay stable:

- /userdata/start_ai.sh: current 2x2 AI display without encoder.
- /userdata/start_ai_enc.sh: current 2x2 AI display with H.265 encoder.

New AVM script:

- /userdata/start_avm.sh: starts the new OEM AVM UI without encoder by default.

Optional later script:

- /userdata/start_avm_enc.sh: starts OEM AVM UI with encoder enabled after the non-encoder AVM path is stable.

The new script should set an environment variable such as OEM_AVM_MODE=1. The main binary can remain the same.

## Interaction Model

The screen does not support touch. A USB mouse is available.

First version supports:

- Mouse click on the bottom toolbar to switch view modes.
- Keyboard number keys 1 through 7 for development and fallback switching.
- Environment variable default selection, for example OEM_AVM_VIEW=surround-main.

## View Modes

The first version will implement seven modes:

1. surround-main
   - Left side: car-centered surround presentation.
   - Right side: selected main camera large view.
   - Default mode.

2. surround-full
   - Full-screen pseudo-3D surround view.
   - Center vehicle placeholder.
   - Four camera textures arranged around the vehicle with perspective-like transforms.

3. ront
   - Full or dominant front camera view.
   - Simple guide lines overlay.

4. ear
   - Full or dominant rear camera view.
   - Simple guide lines overlay.

5. left
   - Left-side focused camera view.

6. ight
   - Right-side focused camera view.

7. multi
   - Multi-panel view inspired by the reference display.
   - Useful for comparing side/front/rear feeds.

Camera-to-view mapping should be configurable because the physical camera wiring may change.

## Visual Layout

The display is landscape. The toolbar is horizontal at the bottom, matching the useful reference orientation.

Toolbar requirements:

- Black or dark translucent bar across the bottom.
- White simplified icons drawn with GLES primitives.
- Red active selection block behind the current mode.
- Clear spacing between mode icons.
- No external font or icon dependency in the first version.

Surround presentation requirements:

- Use a simplified vehicle shape drawn with GLES primitives for the first version.
- Surround camera images should be arranged around the vehicle instead of presented as a plain 2x2 grid.
- The result should suggest OEM AVM style even before real calibration exists.

## Architecture

Add a new isolated display path inside the display module, rather than modifying the stable grid path.

Suggested structure:

- Keep DISPLAY_MODE_GRID unchanged.
- Remove or stop using the old experimental AVM implementation.
- Add a new DISPLAY_MODE_OEM_AVM mode selected only by OEM_AVM_MODE=1.
- Add a small state object inside display_t for the active AVM view, main camera index, toolbar geometry, and mouse state.
- Reuse the existing stable NV12 texture upload path, including the recent glTexSubImage2D update behavior.

Input handling:

- Extend Wayland input handling to receive pointer clicks and keyboard keys if available in the current shell setup.
- If Wayland keyboard/pointer events are not available immediately, keep environment-variable and keyboard fallback as the initial implementation path, then add mouse click handling next.

Rendering:

- Add separate draw functions for each AVM view.
- Draw camera textures using the same shader path as the grid mode.
- Draw toolbar, vehicle placeholder, icons, and guide lines using the existing OSD flat-color shader or a small helper around it.

## Stability Requirements

- Current stable scripts and behavior must remain unchanged.
- AVM mode must be opt-in only.
- Avoid per-frame heap allocation in the AVM draw path.
- Avoid per-frame texture reallocation; reuse the existing texture update approach.
- Keep encoder optional. First validate AVM without encoder, then validate AVM with encoder.

## Verification Plan

Before considering the first AVM version ready:

1. Build succeeds with the cross toolchain.
2. Current stable 2x2 path still runs:
   ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc -n 300
3. Current stable encoder path still runs:
   ./rk3568_camera -m /userdata/yolov5.rknn -c 4 -n 300
4. New AVM path runs without encoder:
   OEM_AVM_MODE=1 ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc -n 600
5. New AVM path runs with encoder after the non-encoder AVM path is stable:
   OEM_AVM_MODE=1 ./rk3568_camera -m /userdata/yolov5.rknn -c 4 -n 600
6. Visual check on the display confirms toolbar, selected red mode block, and view switching.

## Implementation Order

1. Create a new development branch from the protected stable version.
2. Remove or bypass the old experimental AVM code.
3. Add the new DISPLAY_MODE_OEM_AVM mode flag and startup script.
4. Implement static toolbar and default surround-main view.
5. Add keyboard/env view switching.
6. Add mouse click switching if Wayland pointer events are available.
7. Add remaining view modes one by one.
8. Run stable-path regression tests and AVM long-run tests.
