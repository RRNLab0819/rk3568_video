/* inference.cc - RKNN YOLOv5 inference, aligned with official rknpu2 example
 *
 * Key fixes vs old code:
 *   1. Input type: RKNN_TENSOR_UINT8 (NOT INT8) — official always uses UINT8
 *   2. Removed XOR 0x80 hack — data corruption was root cause of bad detection
 *   3. NCHW/NHWC detection from rknn_query (not hardcoded NHWC)
 *   4. Bilinear RGB letterbox (not nearest-neighbour)
 *   5. Diagnostic dumps: rknn attrs, input PPM, candidate boxes
 *   6. OpenCV reference backend for NV12→RGB comparison
 */
#include "inference.h"
#include "yolov5.h"
#include "postprocess.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <cinttypes>
#include <sys/time.h>
#include <im2d.h>
#include <rga.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

struct inference_s {
    rknn_app_context_t   app_ctx;
    uint8_t             *rgb_buf;       /* model input size RGB buffer */
    uint8_t             *diag_rgb_buf;  /* second RGB buffer for dual-path comparison */
    uint8_t             *nv12_buf;      /* model input size NV12 buffer (RGA dst) */
    int                  rgb_size;
    bool                 flip_input;    /* vertically flip input (camera is mounted inverted) */
    int                  nv12_size;
    float                conf_thresh, nms_thresh;
    bool                 rga_enable;

    /* RGA dma_buf preprocess — pre-allocated at infer_open, reused per frame */
    rga_buffer_handle_t  dst_nv12_handle;  /* importbuffer_virtualaddr of nv12_buf */
    int                  mw, mh;           /* model input width/height (320) */

    /* Timing stats */
    float                rga_ms;
    float                cpu_nv12_rgb_ms;
    float                preprocess_total_ms;
};

/* ------------------------------------------------------------------ */
/* Dump helpers                                                        */
/* ------------------------------------------------------------------ */
static const char *fmt_str(rknn_tensor_format f) {
    switch (f) {
    case RKNN_TENSOR_NCHW: return "NCHW";
    case RKNN_TENSOR_NHWC: return "NHWC";
    case RKNN_TENSOR_NC1HWC2: return "NC1HWC2";
    default: return "?";
    }
}
static const char *type_str(rknn_tensor_type t) {
    switch (t) {
    case RKNN_TENSOR_FLOAT32: return "FLOAT32";
    case RKNN_TENSOR_FLOAT16: return "FLOAT16";
    case RKNN_TENSOR_INT8:    return "INT8";
    case RKNN_TENSOR_UINT8:   return "UINT8";
    case RKNN_TENSOR_INT16:   return "INT16";
    default: return "?";
    }
}
static const char *qnt_str(rknn_tensor_qnt_type q) {
    switch (q) {
    case RKNN_TENSOR_QNT_NONE:              return "NONE";
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC: return "AFFINE_ASYMMETRIC";
    default: return "?";
    }
}

static void dump_tensor_attr(rknn_tensor_attr *a)
{
    fprintf(stderr, "  [%d] name=%s n_dims=%d dims=[%d,%d,%d,%d] n_elems=%d size=%d "
            "fmt=%s type=%s qnt=%s zp=%d scale=%f\n",
            a->index, a->name, a->n_dims,
            a->dims[0], a->dims[1], a->dims[2], a->dims[3],
            a->n_elems, a->size,
            fmt_str(a->fmt), type_str(a->type), qnt_str(a->qnt_type),
            a->zp, a->scale);
}

/* ================================================================== */
/* Model loading — aligned with official init_yolov5_model()           */
/* ================================================================== */
static int load_model(const char *path, rknn_app_context_t *ctx)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[infer] cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    int sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = (char *)malloc(sz);
    if (!d) { fclose(f); return -1; }
    fread(d, 1, sz, f);
    fclose(f);

    int ret = rknn_init(&ctx->rknn_ctx, d, sz, 0, NULL);
    free(d);
    if (ret < 0) { fprintf(stderr, "[infer] rknn_init fail ret=%d\n", ret); return -1; }

    /* Query I/O num */
    ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &ctx->io_num, sizeof(ctx->io_num));
    if (ret != RKNN_SUCC) { fprintf(stderr, "[infer] rknn_query IO_NUM fail\n"); return -1; }
    fprintf(stderr, "[infer] model: input num=%d, output num=%d\n",
            ctx->io_num.n_input, ctx->io_num.n_output);

    /* Query input attrs */
    fprintf(stderr, "[infer] --- input tensors ---\n");
    ctx->input_attrs = (rknn_tensor_attr *)calloc(ctx->io_num.n_input, sizeof(rknn_tensor_attr));
    for (int i = 0; i < ctx->io_num.n_input; i++) {
        ctx->input_attrs[i].index = i;
        ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_INPUT_ATTR,
                         &ctx->input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) { fprintf(stderr, "[infer] query input %d fail\n", i); return -1; }
        dump_tensor_attr(&ctx->input_attrs[i]);
    }

    /* Query output attrs */
    fprintf(stderr, "[infer] --- output tensors ---\n");
    ctx->output_attrs = (rknn_tensor_attr *)calloc(ctx->io_num.n_output, sizeof(rknn_tensor_attr));
    for (int i = 0; i < ctx->io_num.n_output; i++) {
        ctx->output_attrs[i].index = i;
        ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_OUTPUT_ATTR,
                         &ctx->output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) { fprintf(stderr, "[infer] query output %d fail\n", i); return -1; }
        dump_tensor_attr(&ctx->output_attrs[i]);
    }

    /* Determine is_quant (same logic as official) */
    if (ctx->output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
        ctx->output_attrs[0].type != RKNN_TENSOR_FLOAT16) {
        ctx->is_quant = true;
    } else {
        ctx->is_quant = false;
    }

    /* Single-output decoded format (e.g. 320 model) needs no fixup.
     * run_inference() handles both 1-output and 3-output paths. */

    /* Extract model dims with NCHW/NHWC detection */
    if (ctx->input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        fprintf(stderr, "[infer] model input fmt: NCHW\n");
        ctx->model_channel = ctx->input_attrs[0].dims[1];
        ctx->model_height  = ctx->input_attrs[0].dims[2];
        ctx->model_width   = ctx->input_attrs[0].dims[3];
    } else {
        fprintf(stderr, "[infer] model input fmt: NHWC\n");
        ctx->model_height  = ctx->input_attrs[0].dims[1];
        ctx->model_width   = ctx->input_attrs[0].dims[2];
        ctx->model_channel = ctx->input_attrs[0].dims[3];
    }
    fprintf(stderr, "[infer] model dims: %dx%dx%d (HWC), is_quant=%d\n",
            ctx->model_height, ctx->model_width, ctx->model_channel, ctx->is_quant);

    return 0;
}

/* ================================================================== */
/* Bilinear RGB letterbox (CPU, matches official convert_image logic)  */
/* ================================================================== */
static void rgb_letterbox(const uint8_t *src, int sw, int sh,
                          uint8_t *dst, int dw, int dh,
                          letterbox_t *lb, int bg_color)
{
    float scale_w = (float)dw / sw;
    float scale_h = (float)dh / sh;
    float scale = (scale_w < scale_h) ? scale_w : scale_h;
    int rw = (int)(sw * scale);
    int rh = (int)(sh * scale);

    /* Align to 4 / 2 (match official allow_slight_change) */
    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;

    int x_pad = (dw - rw) / 2;
    int y_pad = (dh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad -= x_pad % 2; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad -= y_pad % 2; if (y_pad < 0) y_pad = 0; }

    lb->scale = scale;
    lb->x_pad = x_pad;
    lb->y_pad = y_pad;

    /* Fill background */
    memset(dst, bg_color, dw * dh * 3);

    /* Bilinear resize from src into dst letterbox region */
    float x_ratio = (float)sw / rw;
    float y_ratio = (float)sh / rh;

    for (int dy = 0; dy < rh; dy++) {
        float sy_f = dy * y_ratio;
        int sy = (int)sy_f;
        float y_diff = sy_f - sy;
        if (sy >= sh - 1) { sy = sh - 2; y_diff = 1.0f; }

        int src_row0 = sy * sw * 3;
        int src_row1 = (sy + 1) * sw * 3;
        int dst_row  = ((y_pad + dy) * dw + x_pad) * 3;

        for (int dx = 0; dx < rw; dx++) {
            float sx_f = dx * x_ratio;
            int sx = (int)sx_f;
            float x_diff = sx_f - sx;
            if (sx >= sw - 1) { sx = sw - 2; x_diff = 1.0f; }

            int s00 = src_row0 + sx * 3;
            int s01 = src_row0 + (sx + 1) * 3;
            int s10 = src_row1 + sx * 3;
            int s11 = src_row1 + (sx + 1) * 3;
            int d   = dst_row + dx * 3;

            for (int c = 0; c < 3; c++) {
                float v = src[s00+c] * (1 - x_diff) * (1 - y_diff) +
                          src[s01+c] * x_diff * (1 - y_diff) +
                          src[s10+c] * (1 - x_diff) * y_diff +
                          src[s11+c] * x_diff * y_diff;
                dst[d + c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

/* ================================================================== */
/* NV12 → RGB bilinear letterbox (camera mode)                         */
/* ================================================================== */
static void nv12_letterbox_rgb(const uint8_t *nv12, int sw, int sh, int sstride,
                               uint8_t *rgb, int dw, int dh, letterbox_t *lb)
{
    float scale_w = (float)dw / sw;
    float scale_h = (float)dh / sh;
    float scale = (scale_w < scale_h) ? scale_w : scale_h;
    int rw = (int)(sw * scale);
    int rh = (int)(sh * scale);

    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;
    if (rw <= 0 || rh <= 0) return;

    int x_pad = (dw - rw) / 2;
    int y_pad = (dh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad -= x_pad % 2; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad -= y_pad % 2; if (y_pad < 0) y_pad = 0; }

    lb->scale = scale;
    lb->x_pad = x_pad;
    lb->y_pad = y_pad;

    memset(rgb, 114, dw * dh * 3);

    const uint8_t *yplane  = nv12;
    const uint8_t *uvplane = nv12 + sstride * sh;
    float inv_scale = 1.0f / scale;

    for (int dy = 0; dy < rh; dy++) {
        float syf = (dy + 0.5f) * inv_scale - 0.5f;
        if (syf < 0.0f) syf = 0.0f;
        int sy0 = (int)syf;
        if (sy0 >= sh - 1) sy0 = sh - 2;
        int sy1 = sy0 + 1;
        float wy = syf - sy0;
        if (wy < 0.0f) wy = 0.0f;
        if (wy > 1.0f) wy = 1.0f;

        const uint8_t *yrow0 = yplane + sy0 * sstride;
        const uint8_t *yrow1 = yplane + sy1 * sstride;
        uint8_t *drow = rgb + ((y_pad + dy) * dw + x_pad) * 3;

        for (int dx = 0; dx < rw; dx++) {
            float sxf = (dx + 0.5f) * inv_scale - 0.5f;
            if (sxf < 0.0f) sxf = 0.0f;
            int sx0 = (int)sxf;
            if (sx0 >= sw - 1) sx0 = sw - 2;
            int sx1 = sx0 + 1;
            float wx = sxf - sx0;
            if (wx < 0.0f) wx = 0.0f;
            if (wx > 1.0f) wx = 1.0f;

            float y00 = yrow0[sx0], y01 = yrow0[sx1];
            float y10 = yrow1[sx0], y11 = yrow1[sx1];
            int Y = (int)(y00 * (1.0f - wx) * (1.0f - wy) +
                          y01 * wx * (1.0f - wy) +
                          y10 * (1.0f - wx) * wy +
                          y11 * wx * wy + 0.5f);

            int uvx = sx0 & ~1;
            if (uvx >= sw - 1) uvx = sw - 2;
            int uvy = sy0 / 2;
            if (uvy >= sh / 2) uvy = sh / 2 - 1;
            const uint8_t *uvrow = uvplane + uvy * sstride;
            int U = uvrow[uvx];
            int V = uvrow[uvx + 1];

            int C = Y - 16, D = U - 128, E = V - 128;
            int rv = (298 * C + 409 * E + 128) >> 8;
            int gv = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int bv = (298 * C + 516 * D + 128) >> 8;
            drow[0] = (rv < 0) ? 0 : (rv > 255 ? 255 : rv);
            drow[1] = (gv < 0) ? 0 : (gv > 255 ? 255 : gv);
            drow[2] = (bv < 0) ? 0 : (bv > 255 ? 255 : bv);
            drow += 3;
        }
    }
}

/* ================================================================== */
/* Preprocess backend selection (env PREPROCESS_BACKEND)                */
/* ================================================================== */
typedef enum {
    BACKEND_CURRENT_CPU,   /* current CPU NV12→RGB full-res→letterbox */
    BACKEND_CURRENT_RGA,   /* current RGA dma_buf resize→NV12→RGB */
    BACKEND_OPENCV_BGR,    /* OpenCV cvtColor NV12→BGR→resize→letterbox */
    BACKEND_OPENCV_RGB,    /* OpenCV cvtColor NV12→RGB→resize→letterbox */
    BACKEND_NV21_TEST,     /* NV21→RGB (U/V swapped) to test camera output format */
    BACKEND_FLIPV,         /* CURRENT_CPU + vertical flip — test camera orientation */
} preprocess_backend_t;

static preprocess_backend_t detect_backend(void)
{
    static int checked = 0;
    static preprocess_backend_t backend = BACKEND_CURRENT_CPU;
    if (checked) return backend;
    checked = 1;
    const char *e = getenv("PREPROCESS_BACKEND");
    if (!e) return backend;
    if (!strcmp(e, "opencv_bgr"))      backend = BACKEND_OPENCV_BGR;
    else if (!strcmp(e, "opencv_rgb")) backend = BACKEND_OPENCV_RGB;
    else if (!strcmp(e, "current_cpu")) backend = BACKEND_CURRENT_CPU;
    else if (!strcmp(e, "current_rga")) backend = BACKEND_CURRENT_RGA;
    else if (!strcmp(e, "nv21_test"))  backend = BACKEND_NV21_TEST;
    else if (!strcmp(e, "flipv"))      backend = BACKEND_FLIPV;
    fprintf(stderr, "[infer] preprocess backend: %s (%d)\n", e, (int)backend);
    return backend;
}

/* ================================================================== */
/* Color stats diagnostic                                              */
/* ================================================================== */
static void dump_color_stats(const char *tag, const uint8_t *rgb, int w, int h)
{
    long long sum[3] = {0, 0, 0};
    int minv[3] = {255, 255, 255};
    int maxv[3] = {0, 0, 0};
    int n = w * h;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 3; c++) {
            int v = rgb[i * 3 + c];
            sum[c] += v;
            if (v < minv[c]) minv[c] = v;
            if (v > maxv[c]) maxv[c] = v;
        }
    }
    /* Center 64x64 region */
    long long csum[3] = {0, 0, 0};
    int cmin[3] = {255, 255, 255}, cmax[3] = {0, 0, 0};
    int cx0 = w / 2 - 32, cy0 = h / 2 - 32;
    for (int y = cy0; y < cy0 + 64; y++) {
        for (int x = cx0; x < cx0 + 64; x++) {
            int i = y * w + x;
            for (int c = 0; c < 3; c++) {
                int v = rgb[i * 3 + c];
                csum[c] += v;
                if (v < cmin[c]) cmin[c] = v;
                if (v > cmax[c]) cmax[c] = v;
            }
        }
    }
    fprintf(stderr, "[color:%s] full %dx%d mean=(%.1f,%.1f,%.1f) min=(%d,%d,%d) max=(%d,%d,%d) "
            "| center64 mean=(%.1f,%.1f,%.1f) min=(%d,%d,%d) max=(%d,%d,%d)\n",
            tag, w, h,
            sum[0]/(float)n, sum[1]/(float)n, sum[2]/(float)n,
            minv[0], minv[1], minv[2], maxv[0], maxv[1], maxv[2],
            csum[0]/4096.f, csum[1]/4096.f, csum[2]/4096.f,
            cmin[0], cmin[1], cmin[2], cmax[0], cmax[1], cmax[2]);
}

/* ================================================================== */
/* OpenCV NV12→RGB/BGR letterbox (reference for comparison)             */
/* ================================================================== */
static void opencv_nv12_letterbox(const uint8_t *nv12, int sw, int sh,
                                   uint8_t *rgb, int dw, int dh,
                                   letterbox_t *lb, bool to_rgb)
{
    cv::Mat nv12_mat(sh + sh / 2, sw, CV_8UC1, (void *)nv12);
    cv::Mat color;
    int code = to_rgb ? cv::COLOR_YUV2RGB_NV12 : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(nv12_mat, color, code);

    float swf = (float)dw / sw, shf = (float)dh / sh;
    float scale = (swf < shf) ? swf : shf;
    int rw = (int)(sw * scale), rh = (int)(sh * scale);
    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;
    int x_pad = (dw - rw) / 2, y_pad = (dh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad--; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad--; if (y_pad < 0) y_pad = 0; }

    lb->scale = scale;
    lb->x_pad = x_pad;
    lb->y_pad = y_pad;

    cv::Mat resized;
    cv::resize(color, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);

    memset(rgb, 114, dw * dh * 3);
    for (int y = 0; y < rh; y++) {
        const uint8_t *src_row = resized.ptr<const uint8_t>(y);
        memcpy(rgb + ((y_pad + y) * dw + x_pad) * 3, src_row, rw * 3);
    }
}

/* ================================================================== */
/* Dump RGB buffer as PPM for visual inspection                        */
/* ================================================================== */
/* NV21→RGB letterbox  (U/V swapped — tests if camera outputs NV21)     */
/* ================================================================== */
static void nv21_letterbox_rgb(const uint8_t *nv21, int sw, int sh, int sstride,
                                uint8_t *rgb, int dw, int dh, letterbox_t *lb)
{
    uint8_t *full_rgb = (uint8_t *)malloc(sw * sh * 3);
    if (!full_rgb) return;

    const uint8_t *yplane  = nv21;
    const uint8_t *uvplane = nv21 + sstride * sh;

    for (int r = 0; r < sh; r++) {
        const uint8_t *yrow  = yplane  + r * sstride;
        const uint8_t *uvrow = uvplane + (r / 2) * sstride;
        uint8_t *drow = full_rgb + r * sw * 3;
        for (int c = 0; c < sw; c++) {
            int Y = yrow[c];
            int V = uvrow[(c / 2) * 2];      /* NV21: V first */
            int U = uvrow[(c / 2) * 2 + 1];  /* NV21: U second */
            int C = Y - 16, D = U - 128, E = V - 128;
            int rv = (298 * C + 409 * E + 128) >> 8;
            int gv = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int bv = (298 * C + 516 * D + 128) >> 8;
            drow[0] = (rv < 0) ? 0 : (rv > 255 ? 255 : rv);
            drow[1] = (gv < 0) ? 0 : (gv > 255 ? 255 : gv);
            drow[2] = (bv < 0) ? 0 : (bv > 255 ? 255 : bv);
            drow += 3;
        }
    }
    rgb_letterbox(full_rgb, sw, sh, rgb, dw, dh, lb, 114);
    free(full_rgb);
}

/* ================================================================== */
/* Vertical flip helper                                                 */
/* ================================================================== */
static void flip_rgb_vertical(uint8_t *rgb, int w, int h)
{
    uint8_t *tmp = (uint8_t *)malloc(w * 3);
    if (!tmp) return;
    for (int y = 0; y < h / 2; y++) {
        uint8_t *row_a = rgb + y * w * 3;
        uint8_t *row_b = rgb + (h - 1 - y) * w * 3;
        memcpy(tmp, row_a, w * 3);
        memcpy(row_a, row_b, w * 3);
        memcpy(row_b, tmp, w * 3);
    }
    free(tmp);
}

/* ================================================================== */
static void dump_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "[diag] cannot open %s\n", path); return; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, fp);
    fclose(fp);
    fprintf(stderr, "[diag] wrote %s (%dx%d, %d bytes)\n", path, w, h, w * h * 3);
}

/* ================================================================== */
/* Run RKNN inference + postprocess on prepared RGB input              */
/* ================================================================== */
static int run_inference(infer_t *inf, const uint8_t *rgb,
                         const letterbox_t *lb,
                         detection_t *dets, int max_dets)
{
    rknn_app_context_t *ctx = &inf->app_ctx;
    int mw = ctx->model_width, mh = ctx->model_height, mc = ctx->model_channel;

    struct timeval _t0, _t1, _t2, _t3, _t5;
    gettimeofday(&_t0, NULL);

    /* ---- Set input: UINT8 NHWC RGB (aligned with official rknpu2 example) ---- */
    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.type  = RKNN_TENSOR_UINT8;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.size  = mw * mh * mc;
    in.buf   = (void *)rgb;

    int ret = rknn_inputs_set(ctx->rknn_ctx, 1, &in);
    if (ret < 0) { fprintf(stderr, "[infer] rknn_inputs_set fail ret=%d\n", ret); return 0; }
    gettimeofday(&_t1, NULL);

    /* ---- Run ---- */
    ret = rknn_run(ctx->rknn_ctx, NULL);
    if (ret < 0) { fprintf(stderr, "[infer] rknn_run fail ret=%d\n", ret); return 0; }
    gettimeofday(&_t2, NULL);

    /* ---- Get outputs ---- */
    int no = ctx->io_num.n_output;
    rknn_output *out = (rknn_output *)calloc(no, sizeof(rknn_output));
    for (int i = 0; i < no; i++) {
        out[i].index = i;
        out[i].want_float = (!ctx->is_quant);
    }
    ret = rknn_outputs_get(ctx->rknn_ctx, no, out, NULL);
    if (ret < 0) { free(out); return 0; }
    gettimeofday(&_t3, NULL);

    /* ---- Postprocess (dual-path: 3-head NCHW or 1-head decoded) ---- */
    int n = 0;
    int cls[MAX_DETECTIONS];
    float cf[MAX_DETECTIONS], bx[MAX_DETECTIONS], by[MAX_DETECTIONS];
    float bw[MAX_DETECTIONS], bh[MAX_DETECTIONS];

    if (no == 1 && ctx->is_quant) {
        /* Decoded output format [1, N, 85]: x,y,w,h in pixel coords, obj+cls scores.
         * Used by 320x320 (and other single-output) models. No anchor decode needed. */
        int8_t *qbuf = (int8_t *)out[0].buf;
        int n_dets = ctx->output_attrs[0].dims[1];  /* e.g. 6300 */
        int n_props = ctx->output_attrs[0].dims[2]; /* 85 */
        int32_t ozp = ctx->output_attrs[0].zp;
        float oscale = ctx->output_attrs[0].scale;
        float threshold = inf->conf_thresh;

        static int diag_once = 0;
        if (!diag_once) {
            diag_once = 1;
            fprintf(stderr, "[diag] decoded output: %d dets x %d props zp=%d scale=%f\n",
                    n_dets, n_props, ozp, oscale);
            fprintf(stderr, "[diag]   first20 raw: ");
            for (int k = 0; k < 20 && k < n_dets * n_props; k++)
                fprintf(stderr, "%d ", qbuf[k]);
            fprintf(stderr, "\n");
            fprintf(stderr, "[diag] using decoded threshold=%.4f (conf_thresh=%.4f)\n",
                    threshold, inf->conf_thresh);
            fprintf(stderr, "[diag] first 50 dequantized scores: ");
            for (int k = 0; k < 50 && k < n_dets; k++) {
                int off = k * n_props;
                float obj = ((float)qbuf[off + 4] - (float)ozp) * oscale;
                float person_s = ((float)qbuf[off + 5] - (float)ozp) * oscale;
                fprintf(stderr, "%.3f ", obj * person_s);
            }
            fprintf(stderr, "\n");
        }

        /* Direct decode — person-only (class 0). Skip scanning 79 other classes. */
        int candidates = 0;
        for (int i = 0; i < n_dets && candidates < MAX_DETECTIONS; i++) {
            int off = i * n_props;
            float obj    = ((float)qbuf[off + 4] - (float)ozp) * oscale;
            float pscore = ((float)qbuf[off + 5] - (float)ozp) * oscale; /* class 0 = person */
            float score  = obj * pscore;
            if (score >= threshold) {
                float cx = ((float)qbuf[off + 0] - (float)ozp) * oscale;
                float cy = ((float)qbuf[off + 1] - (float)ozp) * oscale;
                float bw_ = ((float)qbuf[off + 2] - (float)ozp) * oscale;
                float bh_ = ((float)qbuf[off + 3] - (float)ozp) * oscale;
                cls[candidates] = 0; /* always person */
                cf[candidates] = score;
                bx[candidates] = cx - bw_ * 0.5f;
                by[candidates] = cy - bh_ * 0.5f;
                bw[candidates] = bw_;
                bh[candidates] = bh_;
                candidates++;
            }
        }

        /* Simple NMS — class 0 (person) only */
        {
            int c = 0;
            for (int i = 0; i < candidates; i++) {
                if (cls[i] != c || cf[i] < threshold) continue;
                for (int j = i + 1; j < candidates; j++) {
                    if (cls[j] != c || cf[j] < threshold) continue;
                    float ix = fmaxf(bx[i], bx[j]);
                    float iy = fmaxf(by[i], by[j]);
                    float iw = fminf(bx[i]+bw[i], bx[j]+bw[j]) - ix;
                    float ih = fminf(by[i]+bh[i], by[j]+bh[j]) - iy;
                    if (iw > 0 && ih > 0) {
                        float inter = iw * ih;
                        float uni = bw[i]*bh[i] + bw[j]*bh[j] - inter;
                        if (uni > 0 && inter / uni > inf->nms_thresh)
                            cf[j] = 0;
                    }
                }
                if (cf[i] >= threshold && n < max_dets) {
                    float ox1 = (bx[i] - lb->x_pad) / lb->scale;
                    float oy1 = (by[i] - lb->y_pad) / lb->scale;
                    float ow  = bw[i] / lb->scale;
                    float oh  = bh[i] / lb->scale;
                    if (ox1 < 0) { ow += ox1; ox1 = 0; }
                    if (oy1 < 0) { oh += oy1; oy1 = 0; }
                    if (ox1 + ow > 1920.0f) ow = 1920.0f - ox1;
                    if (oy1 + oh > 1080.0f) oh = 1080.0f - oy1;
                    if (ow > 0 && oh > 0) {
                        dets[n].class_id = 0;
                        dets[n].confidence = cf[i];
                        dets[n].x = (int)ox1; dets[n].y = (int)oy1;
                        dets[n].w = (int)ow;  dets[n].h = (int)oh;
                        n++;
                    }
                }
            }
        }
    } else {
        /* Traditional 3-head NCHW output: use official postprocess */
        n = post_process_yolov5(ctx, out, (void *)lb,
                                 inf->conf_thresh, inf->nms_thresh,
                                 cls, cf, bx, by, bw, bh, MAX_DETECTIONS);
        if (n > max_dets) n = max_dets;
        for (int i = 0; i < n; i++) {
            dets[i].class_id = cls[i];
            dets[i].confidence = cf[i];
            dets[i].x = (int)bx[i]; dets[i].y = (int)by[i];
            dets[i].w = (int)bw[i]; dets[i].h = (int)bh[i];
        }
    }

    rknn_outputs_release(ctx->rknn_ctx, no, out);
    free(out);
    gettimeofday(&_t5, NULL);

    /* Per-stage timing (every 32nd inference to avoid log spam) */
    {
        static int timing_cnt = 0;
        if ((++timing_cnt & 31) == 0) {
            float set_ms = (_t1.tv_sec - _t0.tv_sec)*1000.0f + (_t1.tv_usec - _t0.tv_usec)/1000.0f;
            float run_ms = (_t2.tv_sec - _t1.tv_sec)*1000.0f + (_t2.tv_usec - _t1.tv_usec)/1000.0f;
            float get_ms = (_t3.tv_sec - _t2.tv_sec)*1000.0f + (_t3.tv_usec - _t2.tv_usec)/1000.0f;
            float pp_ms  = (_t5.tv_sec - _t3.tv_sec)*1000.0f + (_t5.tv_usec - _t3.tv_usec)/1000.0f;
            float tot_ms = (_t5.tv_sec - _t0.tv_sec)*1000.0f + (_t5.tv_usec - _t0.tv_usec)/1000.0f;
            fprintf(stderr, "[perf] input=%.1f run=%.1f get=%.1f post=%.1f total=%.1f ms\n",
                    set_ms, run_ms, get_ms, pp_ms, tot_ms);
        }
    }

    return n;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

extern "C" infer_t *infer_open(const char *model_path, float conf, float nms, bool rga_enable)
{
    infer_t *inf = (infer_t *)calloc(1, sizeof(*inf));
    if (!inf) return NULL;
    inf->conf_thresh = conf;
    inf->nms_thresh  = nms;
    inf->rga_enable  = rga_enable;
    /* Default flip ON — camera sensor is mounted inverted.
     * Set FLIP_INPUT=0 env to disable. */
    {
        const char *f = getenv("FLIP_INPUT");
        inf->flip_input = !(f && f[0] == '0');
    }

    if (load_model(model_path, &inf->app_ctx) < 0) {
        free(inf);
        return NULL;
    }

    inf->rgb_size = inf->app_ctx.model_width * inf->app_ctx.model_height *
                    inf->app_ctx.model_channel;
    inf->rgb_buf = (uint8_t *)malloc(inf->rgb_size);
    if (!inf->rgb_buf) { free(inf); return NULL; }

    /* Second RGB buffer for dual-path comparison */
    inf->diag_rgb_buf = (uint8_t *)malloc(inf->rgb_size);
    if (!inf->diag_rgb_buf) { free(inf->rgb_buf); free(inf); return NULL; }

    /* Pre-allocate NV12 buffer at model input size for RGA resize dst */
    inf->mw = inf->app_ctx.model_width;
    inf->mh = inf->app_ctx.model_height;
    inf->nv12_size = inf->mw * inf->mh * 3 / 2;
    inf->nv12_buf  = (uint8_t *)malloc(inf->nv12_size);
    if (!inf->nv12_buf) { free(inf->rgb_buf); free(inf); return NULL; }

    /* Pre-allocate RGA destination handle (reused across frames) */
    inf->dst_nv12_handle = 0;
    if (inf->rga_enable && inf->nv12_buf) {
        im_handle_param_t dparam;
        memset(&dparam, 0, sizeof(dparam));
        dparam.width  = inf->mw;
        dparam.height = inf->mh;
        dparam.format = RK_FORMAT_YCbCr_420_SP;
        inf->dst_nv12_handle = importbuffer_virtualaddr(inf->nv12_buf, &dparam);
        if (inf->dst_nv12_handle == 0) {
            fprintf(stderr, "[infer] RGA dst import failed, disabling RGA\n");
            inf->rga_enable = false;
        }
    }

    fprintf(stderr, "[infer] ready: model=%s conf=%.2f nms=%.2f "
            "input=%dx%d rga=%d dst_handle=0x%lx rgb_buf=%d nv12_buf=%d\n",
            model_path, conf, nms,
            inf->mw, inf->mh, inf->rga_enable,
            (unsigned long)inf->dst_nv12_handle, inf->rgb_size, inf->nv12_size);
    return inf;
}

/* File mode: RGB888 image → letterbox → RKNN → detections */
extern "C" int infer_detect_rgb(infer_t *inf, const uint8_t *rgb, int w, int h,
                                detection_t *dets, int max_dets)
{
    if (!inf || !rgb || !inf->rgb_buf) return 0;

    int mw = inf->app_ctx.model_width;
    int mh = inf->app_ctx.model_height;
    letterbox_t lb;
    memset(&lb, 0, sizeof(lb));

    /* RGB letterbox → model input size */
    rgb_letterbox(rgb, w, h, inf->rgb_buf, mw, mh, &lb, 114);

    /* Dump input for verification (one-time) */
    {
        static int once = 0;
        if (!once) {
            once = 1;
            fprintf(stderr, "[diag] letterbox: scale=%.4f x_pad=%d y_pad=%d "
                    "src=%dx%d dst=%dx%d\n",
                    lb.scale, lb.x_pad, lb.y_pad, w, h, mw, mh);
            dump_ppm("/tmp/infer_input_file.ppm", inf->rgb_buf, mw, mh);
        }
    }

    return run_inference(inf, inf->rgb_buf, &lb, dets, max_dets);
}

/* Camera mode: NV12 frame_t → RGA dma_buf resize+letterbox → CPU NV12→RGB → RKNN */
extern "C" int infer_detect(infer_t *inf, const frame_t *f,
                            detection_t *dets, int max_dets)
{
    if (!inf || !f || !f->ptr || !inf->rgb_buf || !inf->nv12_buf) return 0;

    int mw = inf->mw, mh = inf->mh;
    int fw = (int)f->width, fh = (int)f->height, fs = (int)f->stride;
    letterbox_t lb;
    memset(&lb, 0, sizeof(lb));

    struct timeval _t0, _t2;
    gettimeofday(&_t0, NULL);

    /* ---- OpenCV backend (bypass RGA/CPU, use cv::cvtColor) ---- */
    preprocess_backend_t backend = detect_backend();
    if (backend == BACKEND_NV21_TEST) {
        nv21_letterbox_rgb((const uint8_t *)f->ptr, fw, fh, fs,
                            inf->rgb_buf, mw, mh, &lb);
        gettimeofday(&_t2, NULL);
        {
            static int nv21_dump = 0;
            if (nv21_dump < 3) {
                char path[64];
                snprintf(path, sizeof(path), "/tmp/input_nv21_test_%d.ppm", nv21_dump);
                dump_ppm(path, inf->rgb_buf, mw, mh);
                dump_color_stats("nv21_test", inf->rgb_buf, mw, mh);
                fprintf(stderr, "[diag] nv21_test dump %d/3: %s scale=%.4f pad=(%d,%d)\n",
                        nv21_dump + 1, path, lb.scale, lb.x_pad, lb.y_pad);
                nv21_dump++;
            }
        }
        if (inf->flip_input) flip_rgb_vertical(inf->rgb_buf, mw, mh);
        return run_inference(inf, inf->rgb_buf, &lb, dets, max_dets);
    }

    if (backend == BACKEND_OPENCV_BGR || backend == BACKEND_OPENCV_RGB) {
        bool to_rgb = (backend == BACKEND_OPENCV_RGB);
        opencv_nv12_letterbox((const uint8_t *)f->ptr, fw, fh,
                               inf->rgb_buf, mw, mh, &lb, to_rgb);
        gettimeofday(&_t2, NULL);

        /* Diagnostic dumps */
        {
            static int ocv_dump = 0;
            if (ocv_dump < 3) {
                char path[64];
                snprintf(path, sizeof(path), "/tmp/input_opencv_%d.ppm", ocv_dump);
                dump_ppm(path, inf->rgb_buf, mw, mh);
                dump_color_stats("opencv", inf->rgb_buf, mw, mh);
                fprintf(stderr, "[diag] opencv dump %d/3: %s letterbox: scale=%.4f pad=(%d,%d)\n",
                        ocv_dump + 1, path, lb.scale, lb.x_pad, lb.y_pad);
                ocv_dump++;
            }
        }
        if (inf->flip_input) flip_rgb_vertical(inf->rgb_buf, mw, mh);
        return run_inference(inf, inf->rgb_buf, &lb, dets, max_dets);
    }

    /* Compute letterbox params */
    float sw = (float)mw / fw, sh = (float)mh / fh;
    float scale = (sw < sh) ? sw : sh;
    int rw = (int)(fw * scale), rh = (int)(fh * scale);
    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;
    int x_pad = (mw - rw) / 2, y_pad = (mh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad -= x_pad % 2; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad -= y_pad % 2; if (y_pad < 0) y_pad = 0; }
    lb.scale = scale; lb.x_pad = x_pad; lb.y_pad = y_pad;

    inf->rga_ms = 0;
    inf->cpu_nv12_rgb_ms = 0;

    if (inf->rga_enable && inf->dst_nv12_handle && f->fd >= 0) {
        /* === RGA dma_buf path: V4L2 fd → importbuffer_fd → improcess NV12→NV12 === */
        struct timeval _rga_t0, _rga_t1;
        gettimeofday(&_rga_t0, NULL);

        /* Fill dst NV12 with letterbox gray */
        memset(inf->nv12_buf, 114, mw * mh);
        memset(inf->nv12_buf + mw * mh, 128, mw * mh / 2);

        /* Import source dma_buf fd */
        im_handle_param_t sparam;
        memset(&sparam, 0, sizeof(sparam));
        sparam.width  = fw;
        sparam.height = fh;
        sparam.format = RK_FORMAT_YCbCr_420_SP;

        rga_buffer_handle_t src_h = importbuffer_fd(f->fd, &sparam);
        bool rga_ok = (src_h != 0);
        if (rga_ok) {
            rga_buffer_t src = wrapbuffer_handle(src_h, fw, fh,
                                                  RK_FORMAT_YCbCr_420_SP,
                                                  fs, fh);
            rga_buffer_t dst = wrapbuffer_handle(inf->dst_nv12_handle, mw, mh,
                                                  RK_FORMAT_YCbCr_420_SP,
                                                  mw, mh);

            im_rect srect = {0, 0, fw, fh};
            im_rect drect = {x_pad, y_pad, rw, rh};
            im_rect prect = {0, 0, 0, 0};
            rga_buffer_t pat;
            memset(&pat, 0, sizeof(pat));

            IM_STATUS rga_ret = improcess(src, dst, pat, srect, drect, prect, 0);
            rga_ok = (rga_ret == IM_STATUS_SUCCESS);
            releasebuffer_handle(src_h);

            if (!rga_ok) {
                static int rga_fail_logged = 0;
                if (!rga_fail_logged) {
                    rga_fail_logged = 1;
                    fprintf(stderr, "[infer] RGA improcess failed: %d, falling back to CPU\n",
                            (int)rga_ret);
                }
            }
        } else {
            static int rga_import_logged = 0;
            if (!rga_import_logged) {
                rga_import_logged = 1;
                fprintf(stderr, "[infer] RGA importbuffer_fd failed (fd=%d), falling back to CPU\n",
                        f->fd);
            }
        }

        gettimeofday(&_rga_t1, NULL);
        inf->rga_ms = (_rga_t1.tv_sec - _rga_t0.tv_sec)*1000.0f +
                      (_rga_t1.tv_usec - _rga_t0.tv_usec)/1000.0f;

        if (rga_ok) {
            /* CPU NV12→RGB from resized 320 NV12 dst */
            struct timeval _cpu_t0, _cpu_t1;
            gettimeofday(&_cpu_t0, NULL);

            const uint8_t *n12 = inf->nv12_buf;
            uint8_t *rgb = inf->rgb_buf;
            for (int r = 0; r < mh; r++) {
                const uint8_t *yrow  = n12 + r * mw;
                const uint8_t *uvrow = n12 + mw * mh + (r / 2) * mw;
                uint8_t *drow = rgb + r * mw * 3;
                for (int c = 0; c < mw; c++) {
                    int Y = yrow[c], U = uvrow[(c/2)*2], V = uvrow[(c/2)*2+1];
                    int C = Y - 16, D = U - 128, E = V - 128;
                    int rv = (298*C + 409*E + 128) >> 8;
                    int gv = (298*C - 100*D - 208*E + 128) >> 8;
                    int bv = (298*C + 516*D + 128) >> 8;
                    drow[0] = (rv < 0) ? 0 : (rv > 255 ? 255 : rv);
                    drow[1] = (gv < 0) ? 0 : (gv > 255 ? 255 : gv);
                    drow[2] = (bv < 0) ? 0 : (bv > 255 ? 255 : bv);
                    drow += 3;
                }
            }

            gettimeofday(&_cpu_t1, NULL);
            inf->cpu_nv12_rgb_ms = (_cpu_t1.tv_sec - _cpu_t0.tv_sec)*1000.0f +
                                   (_cpu_t1.tv_usec - _cpu_t0.tv_usec)/1000.0f;
            gettimeofday(&_t2, NULL);
        } else {
            /* RGA failed — fallback to full CPU path */
            nv12_letterbox_rgb((const uint8_t *)f->ptr, fw, fh, fs,
                               inf->rgb_buf, mw, mh, &lb);
            gettimeofday(&_t2, NULL);
        }
    } else if (inf->rga_enable && f->fd < 0) {
        /* RGA enabled but no dma_buf fd — CPU fallback */
        static int nofd_logged = 0;
        if (!nofd_logged) {
            nofd_logged = 1;
            fprintf(stderr, "[infer] no dma_buf fd (fd=%d), using CPU fallback\n", f->fd);
        }
        nv12_letterbox_rgb((const uint8_t *)f->ptr, fw, fh, fs,
                           inf->rgb_buf, mw, mh, &lb);
        gettimeofday(&_t2, NULL);
    } else {
        /* CPU-only path (stable, no RGA dependency) */
        nv12_letterbox_rgb((const uint8_t *)f->ptr, fw, fh, fs,
                           inf->rgb_buf, mw, mh, &lb);
        gettimeofday(&_t2, NULL);
    }

    /* Apply default vertical flip — camera sensor is mounted inverted */
    if (inf->flip_input)
        flip_rgb_vertical(inf->rgb_buf, mw, mh);

    /* Preprocess timing (every 32nd frame) */
    {
        static int cnt = 0;
        if ((++cnt & 31) == 0) {
            float prep_ms = (_t2.tv_sec - _t0.tv_sec)*1000.0f + (_t2.tv_usec - _t0.tv_usec)/1000.0f;
            inf->preprocess_total_ms = prep_ms;
            fprintf(stderr, "[prep] %s rga=%.1f cpu=%.1f total=%.1f ms\n",
                    inf->rga_enable ? "rga+dma_buf" : "cpu_only",
                    inf->rga_ms, inf->cpu_nv12_rgb_ms, prep_ms);
        }
    }

    /* Diagnostic dump AFTER flip (first 3 frames) — shows actual model input */
    {
        static int dump_count = 0;
        if (dump_count < 3) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/infer_cam0_%d.ppm", dump_count);
            dump_ppm(path, inf->rgb_buf, mw, mh);
            dump_color_stats("current", inf->rgb_buf, mw, mh);
            fprintf(stderr, "[diag] dump %d/3: %s %dx%d "
                    "letterbox: src=%dx%d scale=%.4f pad=(%d,%d) flip=%d\n",
                    dump_count + 1, path, mw, mh, fw, fh,
                    lb.scale, lb.x_pad, lb.y_pad, inf->flip_input);
            dump_count++;
        }
    }

    int n_dets = run_inference(inf, inf->rgb_buf, &lb, dets, max_dets);

    /* ---- Dual-path comparison (DIAG_DUAL=1, first 5 frames) ---- */
    {
        static int dual_done = 0;
        static int dual_enabled = -1;
        if (dual_enabled == -1) {
            const char *e = getenv("DIAG_DUAL");
            dual_enabled = (e && e[0] == '1') ? 1 : 0;
        }
        if (dual_enabled && dual_done < 5) {
            /* Run OpenCV RGB path on same NV12 data */
            letterbox_t lb2;
            memset(&lb2, 0, sizeof(lb2));
            opencv_nv12_letterbox((const uint8_t *)f->ptr, fw, fh,
                                   inf->diag_rgb_buf, mw, mh, &lb2, true);
            detection_t diag_dets[MAX_DETECTIONS];
            int n2 = run_inference(inf, inf->diag_rgb_buf, &lb2, diag_dets, MAX_DETECTIONS);

            /* Print comparison */
            float cur_best = 0, ocv_best = 0;
            int cur_pers = 0, ocv_pers = 0;
            for (int i = 0; i < n_dets; i++) {
                if (dets[i].class_id == 0) {
                    cur_pers++;
                    if (dets[i].confidence > cur_best) cur_best = dets[i].confidence;
                }
            }
            for (int i = 0; i < n2; i++) {
                if (diag_dets[i].class_id == 0) {
                    ocv_pers++;
                    if (diag_dets[i].confidence > ocv_best) ocv_best = diag_dets[i].confidence;
                }
            }
            fprintf(stderr, "[dual] frame=%d current: persons=%d best=%.3f | opencv_rgb: persons=%d best=%.3f\n",
                    dual_done, cur_pers, cur_best, ocv_pers, ocv_best);

            /* Dump both PPMs for visual comparison */
            {
                char p1[64], p2[64];
                snprintf(p1, sizeof(p1), "/tmp/dual_current_%d.ppm", dual_done);
                snprintf(p2, sizeof(p2), "/tmp/dual_opencv_%d.ppm", dual_done);
                dump_ppm(p1, inf->rgb_buf, mw, mh);
                dump_ppm(p2, inf->diag_rgb_buf, mw, mh);
                dump_color_stats("dual_current", inf->rgb_buf, mw, mh);
                dump_color_stats("dual_opencv", inf->diag_rgb_buf, mw, mh);
            }
            dual_done++;
        }
    }

    return n_dets;
}

extern "C" void infer_input_size(infer_t *inf, int *w, int *h)
{
    if (w) *w = inf->app_ctx.model_width;
    if (h) *h = inf->app_ctx.model_height;
}

extern "C" void infer_close(infer_t *inf)
{
    if (!inf) return;
    free(inf->rgb_buf);
    free(inf->diag_rgb_buf);
    free(inf->nv12_buf);
    if (inf->dst_nv12_handle)
        releasebuffer_handle(inf->dst_nv12_handle);
    if (inf->app_ctx.rknn_ctx)
        rknn_destroy(inf->app_ctx.rknn_ctx);
    free(inf->app_ctx.input_attrs);
    free(inf->app_ctx.output_attrs);
    free(inf);
}
