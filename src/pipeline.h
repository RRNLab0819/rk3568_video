#ifndef PIPELINE_H
#define PIPELINE_H

#include "frame.h"
#include "capture.h"
#include "encoder.h"

#define MAX_CAMS 4

typedef struct {
    char     device[64];
    int      width, height, fps;
    uint32_t format;
} capture_cfg_t;

typedef struct {
    int      width, height, fps, bitrate, gop;
    char     codec[8];
} encoder_cfg_t;

typedef struct {
    char     model[256];
    int      interval;
    float    conf, nms;
    bool     person_only;
    float    person_conf;
    bool     smooth_enable;
    float    smooth_alpha;
    int      min_persist;
    char     channels[16];   /* e.g. "0,1,2,3" */
    bool     round_robin;
    bool     rga_preprocess;  /* false=CPU fallback only */
} inference_cfg_t;

typedef struct pipeline_s pipeline_t;

pipeline_t *pipe_new(int n, capture_cfg_t *cam, encoder_cfg_t *enc, int max_frames);
int  pipe_start(pipeline_t *p);
void pipe_stop(pipeline_t *p);
bool pipe_done(pipeline_t *p);

/* Get display ring for camera idx (called from main thread) */
ring_t *pipe_display_ring(pipeline_t *p, int cam_idx);
void pipe_set_inference(pipeline_t *p, inference_cfg_t *cfg);
int  pipe_get_detections(pipeline_t *p, detection_t *dets, int max_dets);
void pipe_print_stats(pipeline_t *p);
void pipe_stats_disp_tick(pipeline_t *p);

#endif
