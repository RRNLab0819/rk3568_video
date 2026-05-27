# OEM AVM UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Build an opt-in OEM-style AVM UI with mouse/keyboard mode switching while leaving the stable 2x2 display flows unchanged.

**Architecture:** Keep the stable grid rendering path untouched and add a new DISPLAY_MODE_OEM_AVM path selected only by OEM_AVM_MODE=1. Remove the old experimental AVM mesh path and replace it with simple GLES layouts, toolbar OSD drawing, and configurable view selection.

**Tech Stack:** C, Wayland client input, EGL/GLES2, existing NV12 texture upload path, shell startup scripts.

---

### Task 1: Branch And Mode Isolation

**Files:**
- Modify: src/display.c
- Create: docs/superpowers/plans/2026-05-27-oem-avm-ui-implementation.md

- [ ] Create codex/oem-avm-ui from the current stable code.
- [ ] Replace the old DISPLAY_MODE_AVM enum entry with DISPLAY_MODE_OEM_AVM.
- [ ] Replace AVM_MODE selection with OEM_AVM_MODE selection.
- [ ] Remove old vm_vbo_* mesh fields and old draw_avm_mode dependency on fisheye AVM meshes.
- [ ] Build after the mode isolation.

### Task 2: OEM AVM State And Input

**Files:**
- Modify: src/display.c

- [ ] Add an internal AVM view enum for surround-main, surround-full, ront, ear, left, ight, and multi.
- [ ] Add active view, main camera mapping, toolbar hit boxes, Wayland pointer, keyboard, and seat handles to display_t.
- [ ] Bind wl_seat from the Wayland registry and install pointer/keyboard listeners.
- [ ] Map keyboard keys 1 through 7 to AVM views.
- [ ] Map mouse clicks on the toolbar to AVM views.

### Task 3: Toolbar And OSD Primitives

**Files:**
- Modify: src/display.c

- [ ] Add reusable GLES OSD helpers for filled rectangles, line rectangles, line segments, and simplified icons.
- [ ] Render a bottom toolbar only in OEM AVM mode.
- [ ] Draw a red active block for the current view and white line icons for all modes.

### Task 4: OEM AVM View Layouts

**Files:**
- Modify: src/display.c

- [ ] Add a helper to draw one camera texture into any normalized rectangle.
- [ ] Implement surround-main as left pseudo-surround plus right large main camera.
- [ ] Implement surround-full as a car-centered pseudo-3D surround layout.
- [ ] Implement ront, ear, left, and ight single/dominant camera layouts.
- [ ] Implement multi as a multi-panel layout.
- [ ] Add simple guide lines for front and rear style views.

### Task 5: Startup Scripts And Verification

**Files:**
- Create: start_avm.sh
- Optionally create: start_avm_enc.sh
- Modify: no stable startup script behavior

- [ ] Add /userdata/start_avm.sh source script with OEM_AVM_MODE=1 and --no-enc.
- [ ] Add /userdata/start_avm_enc.sh source script with OEM_AVM_MODE=1 and encoder enabled.
- [ ] Build and deploy the binary and AVM scripts.
- [ ] Verify stable 2x2 without encoder.
- [ ] Verify stable 2x2 with encoder.
- [ ] Verify OEM AVM without encoder for a long run.
- [ ] Verify OEM AVM with encoder for a shorter smoke run.
