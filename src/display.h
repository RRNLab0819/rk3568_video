/* src/display.h - Wayland/EGL/GLES2 display with dma_buf zero-copy import */
#ifndef DISPLAY_H
#define DISPLAY_H
#include "frame.h"

typedef struct display_s display_t;

display_t *disp_open(int width, int height, int n_cameras);
void disp_update(display_t *d, int cam_idx, const frame_t *f);
void disp_draw(display_t *d);
void disp_dispatch(display_t *d);
void disp_close(display_t *d);
void disp_set_detections(display_t *d, const detection_t *dets, int n_dets);

#endif
