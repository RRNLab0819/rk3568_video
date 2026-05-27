# RKNN Inference Module — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) to implement this plan task-by-task.

**Goal:** Add NPU inference (cam0 only, every N frames) with RGA preprocessing, independent thread, detection overlay on display. Zero impact on 25fps capture/encode/display.

**Architecture:** New inference thread pulls latest frame from `infer_ring` (depth=1), runs RGA NV12→RGB resize, runs RKNN YOLOv5n, writes `detection_t[]` to mutex-protected buffer. Display thread reads buffer and draws bounding boxes as GL quads on cam0.

**Tech Stack:** C + librknnrt + librga + pthread

**Files to modify/create:**
- Create: `src/inference.c` + `src/inference.h`
- Modify: `src/pipeline.c` + `src/pipeline.h` (add infer_ring and infer thread)
- Modify: `src/display.c` (add OSD box drawing)
- Modify: `src/main.c` (add -m flag, wire inference)
- Modify: `src/Makefile` (compile inference.c)

---
### Task 1: Create inference.c/h — RKNN wrapper module

**Files:**
- Create: `src/inference.h`
- Create: `src/inference.c`

- [ ] **Step 1: inference.h — public API**

```c
/* src/inference.h - RKNN YOLOv5n inference with RGA preprocessing */
#ifndef INFERENCE_H
#define INFERENCE_H

#include "frame.h"

typedef struct inference_s infer_t;

/* Create: load model, init RGA, allocate RGB buffer.
 * model_path: .rknn file;  interval: infer every N frames (in capture thread)
 * conf: confidence threshold;  nms: NMS IoU threshold */
infer_t *infer_open(const char *model_path, float conf, float nms);
/* Run: takes NV12 frame_t, blocks ~40ms (RGA+NPU), fills dets.
 * Returns number of detections. Call from inference thread only. */
int  infer_detect(infer_t *inf, const frame_t *f, detection_t *dets, int max_dets);
/* Get model input dimensions (for coordinate mapping) */
void infer_input_size(infer_t *inf, int *w, int *h);
void infer_close(infer_t *inf);

#endif
```

- [ ] **Step 2: inference.c — RKNN init + YOLOv5 postprocess**

Full implementation (~220 lines):

```c
/* src/inference.c - RKNN YOLOv5n + RGA NV12→RGB preprocessing */
#include "inference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <rknn/rknn_api.h>
#include <rga/RgaApi.h>

struct inference_s {
    rknn_context   ctx;
    int            model_w, model_h, model_sz;
    int            n_outputs;
    float          conf_thresh, nms_thresh;
    /* RGA destination: RGB buffer at model resolution */
    uint8_t       *rgb_buf;
    int            rgb_size;
};

/* ---- model loading ---- */
static void *load_file(const char *path, size_t *sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *d = malloc(*sz);
    if (d) fread(d, 1, *sz, f);
    fclose(f);
    return d;
}

/* ---- YOLOv5 postprocess ---- */
static inline float sigmoid(float x) { return 1.0f/(1.0f+expf(-x)); }
static inline float deq(int8_t v, int zp, float s) { return (v-zp)*s; }

static float box_iou(const detection_t *a, const detection_t *b)
{
    float ax1=a->x, ay1=a->y, ax2=a->x+a->w, ay2=a->y+a->h;
    float bx1=b->x, by1=b->y, bx2=b->x+b->w, by2=b->y+b->h;
    float ix1=fmaxf(ax1,bx1), iy1=fmaxf(ay1,by1);
    float ix2=fminf(ax2,bx2), iy2=fminf(ay2,by2);
    float iw=fmaxf(0,ix2-ix1), ih=fmaxf(0,iy2-iy1);
    float inter=iw*ih, ua=(ax2-ax1)*(ay2-ay1)+(bx2-bx1)*(by2-by1)-inter;
    return inter/(ua+1e-6f);
}

static int nms(detection_t *d, int n, float th)
{
    if (n<=1) return n;
    for (int i=0;i<n-1;i++) for (int j=i+1;j<n;j++)
        if (d[j].confidence>d[i].confidence) { detection_t t=d[i];d[i]=d[j];d[j]=t; }
    int keep=0; bool kept[MAX_DETECTIONS]={0};
    for (int i=0;i<n;i++) {
        bool suppress=false;
        for (int j=0;j<i&&!suppress;j++)
            if (kept[j]&&d[i].class_id==d[j].class_id&&box_iou(&d[i],&d[j])>th)
                suppress=true;
        if (!suppress) { d[keep++]=d[i]; kept[i]=true; }
    }
    return keep;
}

static const float ANCHORS[3][3][2]={
    {{10,13},{16,30},{33,23}}, {{30,61},{62,45},{59,119}}, {{116,90},{156,198},{373,326}}};

static void process_stride(const int8_t *data, int stride, int zp, float scale,
                            int src_w, int src_h, detection_t *d, int *n, int max,
                            float conf_th)
{
    int g=(stride==8)?80:(stride==16)?40:20, si=(stride==8)?0:(stride==16)?1:2;
    for (int gy=0;gy<g&&*n<max;gy++) for (int gx=0;gx<g&&*n<max;gx++)
        for (int a=0;a<3&&*n<max;a++) {
            int base=(85*g*g)*a+g*gy+gx, fhw=g*g;
            float conf=sigmoid(deq(data[base+4*fhw],zp,scale));
            if (conf<conf_th) continue;
            float best=0; int bid=0;
            for (int c=0;c<80;c++) {
                float cv=sigmoid(deq(data[base+(5+c)*fhw],zp,scale));
                if (cv>best){best=cv;bid=c;}
            }
            float score=conf*best;
            if (score<conf_th) continue;
            float x=(sigmoid(deq(data[base+0*fhw],zp,scale))*2-0.5+gx)*stride*(float)src_w/640.0f;
            float y=(sigmoid(deq(data[base+1*fhw],zp,scale))*2-0.5+gy)*stride*(float)src_h/640.0f;
            float w=powf(sigmoid(deq(data[base+2*fhw],zp,scale))*2,2)*ANCHORS[si][a][0]*(float)src_w/640.0f;
            float h=powf(sigmoid(deq(data[base+3*fhw],zp,scale))*2,2)*ANCHORS[si][a][1]*(float)src_h/640.0f;
            d[*n].x=(int)fmaxf(0,x-w/2); d[*n].y=(int)fmaxf(0,y-h/2);
            d[*n].w=(int)w; d[*n].h=(int)h;
            d[*n].confidence=score; d[*n].class_id=bid;
            (*n)++;
        }
}

/* ---- public API ---- */

infer_t *infer_open(const char *model_path, float conf, float nms)
{
    infer_t *inf = calloc(1,sizeof(*inf));
    if (!inf) return NULL;
    inf->conf_thresh=conf; inf->nms_thresh=nms;

    size_t sz; void *md = load_file(model_path, &sz);
    if (!md) { free(inf); return NULL; }
    int ret = rknn_init(&inf->ctx, md, sz, 0, NULL);
    free(md);
    if (ret) { free(inf); return NULL; }

    rknn_input_output_num io;
    rknn_query(inf->ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    inf->n_outputs = io.n_output;

    rknn_tensor_attr in_attr = {.index=0};
    rknn_query(inf->ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    inf->model_w = in_attr.dims[2]; inf->model_h = in_attr.dims[1];
    inf->model_sz = in_attr.size; /* NHWC: W*H*3 */

    /* Allocate RGB buffer for RGA output */
    inf->rgb_size = inf->model_w * inf->model_h * 3;
    inf->rgb_buf = malloc(inf->rgb_size);

    printf("[infer] model %dx%d, %d outputs, %d bytes\n",
           inf->model_w, inf->model_h, inf->n_outputs, inf->model_sz);
    return inf;
}

int infer_detect(infer_t *inf, const frame_t *f, detection_t *dets, int max_dets)
{
    if (!inf || !f || !f->ptr || !inf->rgb_buf) return 0;

    /* Step 1: RGA NV12 1920×1080 → RGB model_W×model_H */
    rga_info_t src, dst;
    memset(&src,0,sizeof(src)); memset(&dst,0,sizeof(dst));
    rga_set_rect(&src.rect, 0,0, f->width,f->height,
                 f->stride,f->height, RK_FORMAT_YCbCr_420_SP);
    src.virAddr = f->ptr; src.mmuFlag = 1;
    rga_set_rect(&dst.rect, 0,0, inf->model_w,inf->model_h,
                 inf->model_w,inf->model_h, RK_FORMAT_RGB_888);
    dst.virAddr = inf->rgb_buf; dst.mmuFlag = 1;
    c_RkRgaBlit(&src, &dst, NULL);

    /* Step 2: RKNN inference */
    rknn_input in = {.index=0, .buf=inf->rgb_buf, .size=inf->model_sz,
                      .fmt=RKNN_TENSOR_NHWC, .type=RKNN_TENSOR_UINT8};
    rknn_inputs_set(inf->ctx, 1, &in);
    rknn_run(inf->ctx, NULL);

    /* Step 3: Get outputs */
    rknn_output outs[3]; memset(outs,0,sizeof(outs));
    for (int i=0;i<inf->n_outputs;i++){outs[i].index=i;outs[i].want_float=0;}
    rknn_outputs_get(inf->ctx, inf->n_outputs, outs, NULL);

    /* Step 4: Postprocess */
    int n=0; int strides[3]={8,16,32};
    for (int i=0;i<inf->n_outputs&&i<3;i++) {
        rknn_tensor_attr attr={.index=i};
        rknn_query(inf->ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        process_stride((int8_t*)outs[i].buf, strides[i], attr.zp, attr.scale,
                       f->width, f->height, dets, &n, max_dets, inf->conf_thresh);
    }
    rknn_outputs_release(inf->ctx, inf->n_outputs, outs);

    return nms(dets, n, inf->nms_thresh);
}

void infer_input_size(infer_t *inf, int *w, int *h)
{ if(w)*w=inf->model_w; if(h)*h=inf->model_h; }

void infer_close(infer_t *inf)
{
    if (!inf) return;
    rknn_destroy(inf->ctx);
    free(inf->rgb_buf);
    free(inf);
}
```

- [ ] **Step 3: Build verification**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make build/inference.o
```
Expected: compile OK, zero warnings.

---

### Task 2: Add infer ring + infer thread to pipeline

**Files:**
- Modify: `src/pipeline.h` — add `pipe_infer_ring()` and infer_cfg_t
- Modify: `src/pipeline.c` — add infer ring, infer thread, capture push

- [ ] **Step 1: pipeline.h — add infer_cfg_t and pipe_infer_ring**

Add after encoder_cfg_t:
```c
typedef struct {
    char     model[256];
    int      interval;
    float    conf, nms;
} inference_cfg_t;
```

Add after pipe_display_ring declaration:
```c
/* Get detection results for display overlay (call from main thread, no lock needed for read) */
int  pipe_get_detections(pipeline_t *p, detection_t *dets, int max_dets);
ring_t *pipe_infer_ring(pipeline_t *p);
```

- [ ] **Step 2: pipeline.c — add infer infrastructure**

In struct pipeline_s, add:
```c
    inference_cfg_t *inf_cfg;
    ring_t           infer_ring;          /* capture[0] → infer thread */
    detection_t      dets[MAX_DETECTIONS]; /* inference → display */
    int              det_count;
    pthread_mutex_t  det_lock;
```

In pipe_new, init:
```c
    p->inf_cfg = NULL; /* set by caller after pipe_new if inference enabled */
    pthread_mutex_init(&p->det_lock, NULL);
```

In channel_t, add:
```c
    ring_t        *infer_ring;  /* only cam0, NULL for others */
```

In pipe_new setup:
```c
    if (i == 0) p->ch[i].infer_ring = &p->infer_ring;
    else p->ch[i].infer_ring = NULL;
```

In capture_thread, after ring_put for display, add:
```c
        /* Push to inference ring (cam0 only, every N frames) */
        if (ch->infer_ring && ch->pipe->inf_cfg &&
            (ch->frame_count % ch->pipe->inf_cfg->interval == 0))
            ring_put(ch->infer_ring, &f);
```

Add inference thread function:
```c
static void *infer_thread(void *arg)
{
    pipeline_t *p = arg;
    infer_t *inf = infer_open(p->inf_cfg->model, p->inf_cfg->conf, p->inf_cfg->nms);
    if (!inf) { fprintf(stderr, "[infer] model load failed\n"); return NULL; }

    while (p->running) {
        frame_t f;
        if (!ring_get(&p->infer_ring, &f)) { usleep(10000); continue; }

        detection_t dets[MAX_DETECTIONS];
        int n = infer_detect(inf, &f, dets, MAX_DETECTIONS);

        pthread_mutex_lock(&p->det_lock);
        p->det_count = n;
        memcpy(p->dets, dets, n * sizeof(detection_t));
        pthread_mutex_unlock(&p->det_lock);

        if (f.fd >= 0) close(f.fd);
    }
    infer_close(inf);
    return NULL;
}
```

In pipe_start, after encode threads, start infer thread:
```c
    if (p->inf_cfg) {
        pthread_t t;
        pthread_create(&t, NULL, infer_thread, p);
        pthread_detach(t); /* fire and forget, stops when p->running=false */
    }
```

Add public accessors:
```c
ring_t *pipe_infer_ring(pipeline_t *p) { return &p->infer_ring; }

int pipe_get_detections(pipeline_t *p, detection_t *dets, int max_dets)
{
    pthread_mutex_lock(&p->det_lock);
    int n = p->det_count; if (n > max_dets) n = max_dets;
    memcpy(dets, p->dets, n * sizeof(detection_t));
    pthread_mutex_unlock(&p->det_lock);
    return n;
}
```

- [ ] **Step 3: Build verification**

```bash
make build/pipeline.o
```
Expected: compile OK.

---

### Task 3: Add OSD bounding box drawing to display

**Files:**
- Modify: `src/display.c` — add `disp_set_detections()` and box drawing
- Modify: `src/display.h` — add API

- [ ] **Step 1: display.h — add**

```c
/* Set detection results to overlay on cam0 */
void disp_set_detections(display_t *d, const detection_t *dets, int n_dets);
```

- [ ] **Step 2: display.c — add detection storage and OSD drawing**

In struct display_s, add:
```c
    detection_t  dets[MAX_DETECTIONS];
    int          det_count;
    pthread_mutex_t det_lock;
```

In disp_open, init: `pthread_mutex_init(&d->det_lock, NULL);`

Add disp_set_detections:
```c
void disp_set_detections(display_t *d, const detection_t *dets, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->det_lock);
    d->det_count = (n > MAX_DETECTIONS) ? MAX_DETECTIONS : n;
    memcpy(d->dets, dets, d->det_count * sizeof(detection_t));
    pthread_mutex_unlock(&d->det_lock);
}
```

In disp_draw, after rendering camera quads, add box overlay for cam0 only:
```c
    /* Draw detection boxes on cam0 */
    pthread_mutex_lock(&d->det_lock);
    if (d->det_count > 0 && d->n_cams > 0 && d->has_frame[0]) {
        /* Green box shader (simple color) */
        glUseProgram(d->osd_prog);
        glUniform4f(d->osd_color_loc, 0.0f, 1.0f, 0.0f, 1.0f);
        /* cam0 is top-left: x∈[-1,0], y∈[0,1] for 2×2 */
        float disp_w = (d->n_cams <= 2) ? 2.0f : 1.0f;
        float disp_h = (d->n_cams <= 2) ? 2.0f : 1.0f;
        float x_scale = disp_w / 1920.0f;
        float y_scale = disp_h / 1080.0f;
        float x_off = -1.0f, y_off = (d->n_cams<=2) ? -1.0f : 0.0f;
        for (int i=0; i<d->det_count; i++) {
            float bx = x_off + d->dets[i].x * x_scale;
            float by = y_off + (1080-d->dets[i].y-d->dets[i].h) * y_scale;
            float bw = d->dets[i].w * x_scale;
            float bh = d->dets[i].h * y_scale;
            /* 4 line segments forming a box */
            float verts[]={bx,by, bx+bw,by, bx+bw,by+bh, bx,by+bh};
            glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
            glEnableVertexAttribArray(d->osd_pos);
            glDrawArrays(GL_LINE_LOOP, 0, 4);
            glDisableVertexAttribArray(d->osd_pos);
        }
    }
    pthread_mutex_unlock(&d->det_lock);
```

OSD shader (simple flat color vertex+fragment, add as static const in display.c):
```c
static const char osd_vert[] =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "void main(){ gl_Position=vec4(a_pos,0.0,1.0); }\n";
static const char osd_frag[] =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main(){ gl_FragColor=u_color; }\n";
```

In disp_open, compile OSD program:
```c
    d->osd_prog = glCreateProgram();
    { GLuint v=compile_shader(GL_VERTEX_SHADER,osd_vert);
      GLuint f=compile_shader(GL_FRAGMENT_SHADER,osd_frag);
      glAttachShader(d->osd_prog,v); glAttachShader(d->osd_prog,f);
      glLinkProgram(d->osd_prog); glDeleteShader(v); glDeleteShader(f); }
    d->osd_pos = glGetAttribLocation(d->osd_prog, "a_pos");
    d->osd_color_loc = glGetUniformLocation(d->osd_prog, "u_color");
```

- [ ] **Step 3: Build verification**

```bash
make build/display.o
```

---

### Task 4: Wire inference in main.c + Makefile

**Files:**
- Modify: `src/main.c` — add `-m model` and `--infer-interval`, wire to pipeline
- Modify: `src/Makefile` — add inference.c to CSRCS

- [ ] **Step 1: Makefile — add inference.c**

```makefile
CSRCS := capture.c display.c encoder.c inference.c main.c pipeline.c xdg-shell-client.c
```

- [ ] **Step 2: main.c — wire inference**

After loading CLI args and config, add:
```c
    inference_cfg_t inf_cfg = {0};
    if (model[0]) {
        strncpy(inf_cfg.model, model, 255);
        inf_cfg.interval = infer_interval;
        inf_cfg.conf = infer_conf;
        inf_cfg.nms  = infer_nms;
    }
```

Pass inf_cfg to pipe_new or set after:
```c
    g_pipe = pipe_new(n_cams, cam_cfg, use_enc?&enc_cfg:NULL, max_frames);
    if (model[0]) {
        g_pipe->inf_cfg = malloc(sizeof(inference_cfg_t));
        memcpy(g_pipe->inf_cfg, &inf_cfg, sizeof(inf_cfg));
    }
```
Note: requires making `inf_cfg` field in `pipeline_s` publicly settable. Alternative: add a `pipe_set_inference()` setter.

In main loop, after disp_dispatch, read and push detections:
```c
            detection_t dets[MAX_DETECTIONS];
            int n = pipe_get_detections(g_pipe, dets, MAX_DETECTIONS);
            if (n > 0) disp_set_detections(g_disp, dets, n);
```

- [ ] **Step 3: Add `-m` and `--infer-interval` CLI parsing** (already partially in main.c, verify)

---

### Task 5: Build, deploy, test

- [ ] **Step 1: Full build**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make clean && make
```

- [ ] **Step 2: Deploy**

```bash
adb push rk3568_camera /userdata/rk3568_camera
adb push config.ini /userdata/rk3568-camera/config.ini
```

- [ ] **Step 3: Test — 1 camera + inference**

```bash
adb shell "/userdata/rk3568_camera -c 1 -m /userdata/yolov5n_320.rknn --infer-interval 5 -n 100"
```

Expected:
- Display shows camera + green detection boxes on detected objects
- Console shows `[infer] model 640x640, 3 outputs, 1228800 bytes`
- 25fps capture maintained

- [ ] **Step 4: Test — 4 cameras + inference on cam0 only**

```bash
adb shell "/userdata/rk3568_camera -c 4 -m /userdata/yolov5n_320.rknn --infer-interval 5"
```
Expected: 4 cameras display, boxes only on cam0, encoding threads running.

---

### Coordinate Mapping Summary

```
Model output (640×640)  →  Original frame (1920×1080)  →  Display quad (640×360 in 2×2)
  det.x *= 1920/640       (already done in process_stride)    box.x *= 1.0/1920 (clip space, -1..0)
  det.y *= 1080/640                                         box.y *= 1.0/1080, flipped (OpenGL Y-up)
```

GL Y-axis goes up (0=bottom, 1080=top). NV12 frame Y goes down (0=top, 1080=bottom). Flip: `gl_y = (1080 - y - h) / 1080 * quad_h + quad_y0`.
