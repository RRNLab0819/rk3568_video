#ifndef INFERENCE_H
#define INFERENCE_H

#include "frame.h"

typedef struct inference_s infer_t;

#ifdef __cplusplus
extern "C" {
#endif

infer_t *infer_open(const char *model_path, float conf, float nms, bool rga_enable);
int  infer_detect(infer_t *inf, const frame_t *f, detection_t *dets, int max_dets);
void infer_input_size(infer_t *inf, int *w, int *h);
void infer_close(infer_t *inf);

/* File test mode: feed RGB888 image directly, bypass camera NV12 pipeline */
int  infer_detect_rgb(infer_t *inf, const uint8_t *rgb, int w, int h,
                      detection_t *dets, int max_dets);

#ifdef __cplusplus
}
#endif

#endif
