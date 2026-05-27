# RKNN Inference Fix — Use Official Pre/Post Processing

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) to implement.

**Goal:** Replace custom NV12→RGB resize + YOLO postprocess with official RKNN model zoo's `convert_image_with_letterbox` + `post_process`, producing correct detections with proper coordinate mapping.

**Root Cause:** Custom `nv12_to_rgb` stretches 1920×1080→640×640 without letterbox (distorts aspect ratio). Custom YOLO postprocess uses wrong stride/tensor layout. Both cause zero detections.

**Fix:** Copy official `image_utils.c/h` and `postprocess.cc/h` from model zoo, rewrite `inference.c` to use them via C-compatible wrappers.

**Tech Stack:** C + C++ (postprocess.cc is C++) + librknnrt + official image_utils

---
### Task 1: Copy official utils and adapt to C

**Files:**
- Copy: `~/3568/rknn_model_zoo-1.6.0/utils/image_utils.c` → `src/image_utils.c`
- Copy: `~/3568/rknn_model_zoo-1.6.0/utils/image_utils.h` → `src/image_utils.h`
- Copy: `~/3568/rknn_model_zoo-1.6.0/examples/yolov5/cpp/postprocess.cc` → `src/postprocess.cc`
- Copy: `~/3568/rknn_model_zoo-1.6.0/examples/yolov5/cpp/postprocess.h` → `src/postprocess.h`
- Copy: `~/3568/rknn_model_zoo-1.6.0/utils/image_drawing.c` → `src/image_drawing.c`
- Copy: `~/3568/rknn_model_zoo-1.6.0/utils/image_drawing.h` → `src/image_drawing.h`

- [ ] **Step 1: Copy files**

```bash
SDK=/home/rrn/3568/rknn_model_zoo-1.6.0
DST=/home/rrn/rk3568-camera/src
cp $SDK/utils/image_utils.c $DST/
cp $SDK/utils/image_utils.h $DST/
cp $SDK/examples/yolov5/cpp/postprocess.cc $DST/
cp $SDK/examples/yolov5/cpp/postprocess.h $DST/
cp $SDK/utils/image_drawing.c $DST/
cp $SDK/utils/image_drawing.h $DST/
# Also need the common.h from examples
cp $SDK/examples/yolov5/cpp/yolov5.h $DST/yolov5_model.h
```

- [ ] **Step 2: Adapt postprocess.h — remove C++ vector usage, add C wrapper**

Add at end of `src/postprocess.h`:
```c
#ifdef __cplusplus
extern "C" {
#endif

/* C-compatible wrapper: takes raw float arrays instead of std::vector */
int post_process_yolov5(rknn_app_context_t *ctx, void *outputs,
                         letterbox_t *letter_box, float conf, float nms,
                         int *out_class, float *out_conf,
                         float *out_x, float *out_y, float *out_w, float *out_h,
                         int max_dets);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Add C wrapper in postprocess.cc**

At end of `src/postprocess.cc`, add:
```cpp
extern "C" {

int post_process_yolov5(rknn_app_context_t *ctx, void *outputs,
                         letterbox_t *letter_box, float conf, float nms,
                         int *out_class, float *out_conf,
                         float *out_x, float *out_y, float *out_w, float *out_h,
                         int max_dets)
{
    object_detect_result_list od_results;
    int ret = post_process(ctx, outputs, letter_box, conf, nms, &od_results);
    int n = od_results.count;
    if (n > max_dets) n = max_dets;
    for (int i = 0; i < n; i++) {
        out_class[i] = od_results.results[i].cls_id;
        out_conf[i]  = od_results.results[i].prop;
        out_x[i]     = od_results.results[i].box.left;
        out_y[i]     = od_results.results[i].box.top;
        out_w[i]     = od_results.results[i].box.right - od_results.results[i].box.left;
        out_h[i]     = od_results.results[i].box.bottom - od_results.results[i].box.top;
    }
    return n;
}

} // extern "C"
```

- [ ] **Step 4: Verify files exist**

```bash
ls -la /home/rrn/rk3568-camera/src/image_utils.c /home/rrn/rk3568-camera/src/postprocess.cc
```
Expected: files exist

---

### Task 2: Rewrite inference.c to use official pre/post processing

**Files:**
- Replace: `src/inference.c` (complete rewrite, ~150 lines C)
- Keep: `src/inference.h` (same API)

- [ ] **Step 1: Rewrite inference.c**

The new inference.c uses:
- `init_yolov5_model()` / `release_yolov5_model()` from yolov5.cc (copy relevant parts)
- `convert_image_with_letterbox()` from image_utils.h
- `post_process_yolov5()` (C wrapper from Task 1)

```c
/* src/inference.c - Uses official RKNN model zoo preprocessing + postprocessing */
#include "inference.h"
#include "yolov5_model.h"      /* rknn_app_context_t */
#include "image_utils.h"       /* convert_image_with_letterbox, image_buffer_t */
#include "postprocess.h"       /* post_process_yolov5 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct inference_s {
    rknn_app_context_t   app_ctx;
    image_buffer_t       dst_img;     /* pre-allocated RGB buffer at model resolution */
    float                conf_thresh;
    float                nms_thresh;
};

/* ---- model loading (copied from yolov5.cc init_yolov5_model) ---- */
static int load_model(const char *path, rknn_app_context_t *ctx)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END); int sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = malloc(sz);
    fread(data, 1, sz, f); fclose(f);

    int ret = rknn_init(&ctx->rknn_ctx, data, sz, 0, NULL);
    free(data);
    if (ret) return -1;

    rknn_query(ctx->rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &ctx->io_num, sizeof(ctx->io_num));

    ctx->input_attrs = malloc(ctx->io_num.n_input * sizeof(rknn_tensor_attr));
    for (int i = 0; i < ctx->io_num.n_input; i++) {
        ctx->input_attrs[i].index = i;
        rknn_query(ctx->rknn_ctx, RKNN_QUERY_INPUT_ATTR, &ctx->input_attrs[i],
                   sizeof(rknn_tensor_attr));
    }

    ctx->output_attrs = malloc(ctx->io_num.n_output * sizeof(rknn_tensor_attr));
    for (int i = 0; i < ctx->io_num.n_output; i++) {
        ctx->output_attrs[i].index = i;
        rknn_query(ctx->rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &ctx->output_attrs[i],
                   sizeof(rknn_tensor_attr));
    }

    ctx->is_quant = (ctx->output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    ctx->model_width  = ctx->input_attrs[0].dims[2];  /* NHWC: W=dim[2] */
    ctx->model_height = ctx->input_attrs[0].dims[1];
    ctx->model_channel = ctx->input_attrs[0].dims[3];

    printf("[infer] model %dx%dx%d quant=%d in=%d out=%d\n",
           ctx->model_width, ctx->model_height, ctx->model_channel,
           ctx->is_quant, ctx->io_num.n_input, ctx->io_num.n_output);
    return 0;
}

/* ---- public API ---- */
infer_t *infer_open(const char *model_path, float conf, float nms)
{
    infer_t *inf = calloc(1, sizeof(*inf));
    inf->conf_thresh = conf;
    inf->nms_thresh  = nms;

    if (load_model(model_path, &inf->app_ctx) < 0) {
        free(inf); return NULL;
    }

    /* Pre-allocate RGB buffer at model resolution */
    inf->dst_img.width  = inf->app_ctx.model_width;
    inf->dst_img.height = inf->app_ctx.model_height;
    inf->dst_img.format = IMAGE_FORMAT_RGB888;
    inf->dst_img.size   = inf->dst_img.width * inf->dst_img.height * 3;
    inf->dst_img.virt_addr = malloc(inf->dst_img.size);

    return inf;
}

int infer_detect(infer_t *inf, const frame_t *f, detection_t *dets, int max_dets)
{
    if (!inf || !f || !f->ptr) return 0;

    /* Step 1: Build source image_buffer_t from NV12 frame */
    image_buffer_t src_img;
    memset(&src_img, 0, sizeof(src_img));
    src_img.width  = f->width;
    src_img.height = f->height;
    src_img.format = IMAGE_FORMAT_NV12;
    src_img.size   = f->width * f->height * 3 / 2;
    src_img.virt_addr = (uint8_t *)f->ptr;
    /* Hack: pass stride in unused field so image_utils can handle it */
    /* image_utils expects contiguous NV12 at virt_addr */

    /* Step 2: Convert + letterbox via official function */
    letterbox_t letter_box;
    memset(&letter_box, 0, sizeof(letter_box));
    memset(inf->dst_img.virt_addr, 114, inf->dst_img.size); /* grey fill */

    int ret = convert_image_with_letterbox(&src_img, &inf->dst_img,
                                            &letter_box, 114);
    if (ret < 0) return 0;

    /* Step 3: Set input + run */
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;
    inputs[0].size  = inf->dst_img.size;
    inputs[0].buf   = inf->dst_img.virt_addr;
    rknn_inputs_set(inf->app_ctx.rknn_ctx, 1, inputs);
    rknn_run(inf->app_ctx.rknn_ctx, NULL);

    /* Step 4: Get outputs */
    rknn_output outputs[inf->app_ctx.io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < inf->app_ctx.io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = (!inf->app_ctx.is_quant);
    }
    rknn_outputs_get(inf->app_ctx.rknn_ctx, inf->app_ctx.io_num.n_output,
                     outputs, NULL);

    /* Step 5: Official postprocess (handles letter_box scaling) */
    int cls[MAX_DETECTIONS]; float cf[MAX_DETECTIONS];
    float bx[MAX_DETECTIONS], by[MAX_DETECTIONS];
    float bw[MAX_DETECTIONS], bh[MAX_DETECTIONS];
    int n = post_process_yolov5(&inf->app_ctx, outputs, &letter_box,
                                 inf->conf_thresh, inf->nms_thresh,
                                 cls, cf, bx, by, bw, bh, MAX_DETECTIONS);

    rknn_outputs_release(inf->app_ctx.rknn_ctx, inf->app_ctx.io_num.n_output, outputs);

    /* Step 6: Convert to detection_t (coordinates already in 1920x1080 space) */
    for (int i = 0; i < n && i < max_dets; i++) {
        dets[i].class_id   = cls[i];
        dets[i].confidence = cf[i];
        dets[i].x = (int)bx[i];
        dets[i].y = (int)by[i];
        dets[i].w = (int)bw[i];
        dets[i].h = (int)bh[i];
    }
    return n;
}

void infer_close(infer_t *infer)
{
    if (!infer) return;
    free(infer->dst_img.virt_addr);
    if (infer->app_ctx.rknn_ctx) rknn_destroy(infer->app_ctx.rknn_ctx);
    free(infer->app_ctx.input_attrs);
    free(infer->app_ctx.output_attrs);
    free(infer);
}

void infer_input_size(infer_t *inf, int *w, int *h)
{
    if (w) *w = inf->app_ctx.model_width;
    if (h) *h = inf->app_ctx.model_height;
}
```

- [ ] **Step 2: Build verification**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make build/inference.o postprocess.o image_utils.o
```
Expected: compile OK.

---

### Task 3: Update Makefile for C++ and new sources

**Files:**
- Modify: `src/Makefile` → `/home/rrn/rk3568-camera/Makefile`

- [ ] **Step 1: Add C++ support and new sources**

Change `CC` to `CXX` for C++ files, add new source files:
```makefile
CSRCS    := capture.c display.c encoder.c image_utils.c main.c pipeline.c xdg-shell-client.c
CXXSRCS  := inference.cc postprocess.cc
OBJS     := $(patsubst %.c, build/%.o, $(CSRCS)) $(patsubst %.cc, build/%.o, $(CXXSRCS))
```

Add C++ compile rule:
```makefile
build/%.o: src/%.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

Note: inference.c should be renamed to inference.cc since it includes C++ headers (postprocess.h, image_utils.h are C++).

- [ ] **Step 2: Link with C++ standard library**

```makefile
LIBS += -lstdc++
```

---

### Task 4: Build, deploy, test

- [ ] **Step 1: Full build**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make clean && make
```
Expected: all compile, link OK.

- [ ] **Step 2: Deploy**

```bash
adb push rk3568_camera /userdata/rk3568_camera
```

- [ ] **Step 3: Test**

```bash
adb shell "/userdata/rk3568_camera -c 1 -m /userdata/yolov5n_320.rknn -n 50"
```
Expected: detection boxes visible on cam0.

- [ ] **Step 4: Verify coordinates**

Point camera at a person standing in frame center. Expected: green box centered on person.
