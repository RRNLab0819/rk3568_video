// isolated_yolov5_test_320.cc
// Tests the 320x320 YOLOv5n RKNN model with correct output decoding.
//
// The 320 model exports with a SINGLE decoded output [1, 6300, 85] where:
//   x,y,w,h are already in 320x320 pixel coordinates (NOT raw feature maps)
//   objectness and class scores are already sigmoided
//   Score = objectness * class_score
//
// NO anchor decode, NO sigmoid, NO 3-head split needed!
// This is fundamentally different from the 640 model which has 3 raw NCHW outputs.
//
// Usage: ./isolated_yolov5_test_320 <model.rknn> <image.jpg>
// Build: make -f Makefile.isolated_320

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "rknn_api.h"

// ---- Config ----
#define OBJ_CLASS_NUM 80
#define MAX_DET       128
#define NMS_THRESH    0.45f
#define BOX_THRESH    0.25f

// ---- Letterbox ----
typedef struct {
    float scale;
    int x_pad, y_pad;
} letterbox_t;

static void rgb_letterbox(const uint8_t *src, int sw, int sh,
                          uint8_t *dst, int dw, int dh,
                          letterbox_t *lb, int bg) {
    float sw_f = (float)dw / sw, sh_f = (float)dh / sh;
    float scale = (sw_f < sh_f) ? sw_f : sh_f;
    int rw = (int)(sw * scale), rh = (int)(sh * scale);
    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;
    int x_pad = (dw - rw) / 2, y_pad = (dh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad -= x_pad % 2; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad -= y_pad % 2; if (y_pad < 0) y_pad = 0; }
    lb->scale = scale; lb->x_pad = x_pad; lb->y_pad = y_pad;
    memset(dst, bg, dw * dh * 3);
    float xr = (float)sw / rw, yr = (float)sh / rh;
    for (int dy = 0; dy < rh; dy++) {
        float sy_f = dy * yr; int sy = (int)sy_f;
        float yd = sy_f - sy;
        if (sy >= sh - 1) { sy = sh - 2; yd = 1.0f; }
        int sr0 = sy * sw * 3, sr1 = (sy + 1) * sw * 3;
        int dr = ((y_pad + dy) * dw + x_pad) * 3;
        for (int dx = 0; dx < rw; dx++) {
            float sx_f = dx * xr; int sx = (int)sx_f;
            float xd = sx_f - sx;
            if (sx >= sw - 1) { sx = sw - 2; xd = 1.0f; }
            int s00 = sr0 + sx*3, s01 = sr0 + (sx+1)*3;
            int s10 = sr1 + sx*3, s11 = sr1 + (sx+1)*3;
            int d = dr + dx * 3;
            for (int c = 0; c < 3; c++)
                dst[d+c] = (uint8_t)(src[s00+c]*(1-xd)*(1-yd) + src[s01+c]*xd*(1-yd) +
                                     src[s10+c]*(1-xd)*yd + src[s11+c]*xd*yd + 0.5f);
        }
    }
}

static void dump_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, fp);
    fclose(fp);
    fprintf(stderr, "[test320] wrote %s (%dx%d)\n", path, w, h);
}

// ---- IOU for NMS ----
static float CalcIoU(float x1a, float y1a, float x2a, float y2a,
                     float x1b, float y1b, float x2b, float y2b) {
    float iw = fmaxf(0.f, fminf(x2a, x2b) - fmaxf(x1a, x1b) + 1.f);
    float ih = fmaxf(0.f, fminf(y2a, y2b) - fmaxf(y1a, y1b) + 1.f);
    float inter = iw * ih;
    float uni = (x2a-x1a+1)*(y2a-y1a+1) + (x2b-x1b+1)*(y2b-y1b+1) - inter;
    return (uni > 0.f) ? inter / uni : 0.f;
}

// ---- Main ----
int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <model.rknn> <image.jpg>\n", argv[0]);
        return -1;
    }
    const char *model_path = argv[1], *image_path = argv[2];

    // ---- Load image ----
    int iw, ih, ic;
    unsigned char *pixels = stbi_load(image_path, &iw, &ih, &ic, 3);
    if (!pixels) { fprintf(stderr, "stbi_load fail\n"); return -1; }
    fprintf(stderr, "[test320] image: %dx%d ch=%d\n", iw, ih, ic);

    // ---- Load RKNN model ----
    FILE *fm = fopen(model_path, "rb");
    if (!fm) { fprintf(stderr, "fopen model fail\n"); stbi_image_free(pixels); return -1; }
    fseek(fm, 0, SEEK_END); int mlen = ftell(fm); fseek(fm, 0, SEEK_SET);
    char *mdata = (char *)malloc(mlen); fread(mdata, 1, mlen, fm); fclose(fm);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, mdata, mlen, 0, NULL);
    free(mdata);
    if (ret < 0) { fprintf(stderr, "rknn_init fail ret=%d\n", ret); stbi_image_free(pixels); return -1; }

    // ---- Query model info ----
    rknn_input_output_num io_num;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    fprintf(stderr, "[test320] inputs=%d outputs=%d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    fprintf(stderr, "[test320] INPUT:  name=%s dims=[%d,%d,%d,%d] fmt=%d type=%d qnt=%d zp=%d scale=%f\n",
            in_attr.name, in_attr.dims[0], in_attr.dims[1], in_attr.dims[2], in_attr.dims[3],
            in_attr.fmt, in_attr.type, in_attr.qnt_type, in_attr.zp, in_attr.scale);

    int mw, mh, mc;
    if (in_attr.fmt == RKNN_TENSOR_NCHW) {
        mw = in_attr.dims[3]; mh = in_attr.dims[2]; mc = in_attr.dims[1];
    } else {
        mw = in_attr.dims[2]; mh = in_attr.dims[1]; mc = in_attr.dims[3];
    }
    fprintf(stderr, "[test320] model input: %dx%dx%d\n", mh, mw, mc);

    rknn_tensor_attr out_attr;
    memset(&out_attr, 0, sizeof(out_attr));
    out_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
    fprintf(stderr, "[test320] OUTPUT: name=%s n_dims=%d dims=[%d,%d,%d,%d] fmt=%d type=%d "
            "qnt=%d zp=%d scale=%f n_elems=%d size=%d\n",
            out_attr.name, out_attr.n_dims,
            out_attr.dims[0], out_attr.dims[1], out_attr.dims[2], out_attr.dims[3],
            out_attr.fmt, out_attr.type, out_attr.qnt_type, out_attr.zp, out_attr.scale,
            out_attr.n_elems, out_attr.size);

    int n_dets = out_attr.dims[1];  // 6300
    int n_props = out_attr.dims[2]; // 85
    bool is_quant = (out_attr.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    float out_scale = out_attr.scale;
    int32_t out_zp = out_attr.zp;
    fprintf(stderr, "[test320] output format: %d detections x %d props, is_quant=%d zp=%d scale=%f\n",
            n_dets, n_props, is_quant, out_zp, out_scale);
    if (n_dets <= 0 || n_dets > 100000) {
        fprintf(stderr, "[test320] ERROR: unexpected output dims\n");
        rknn_destroy(ctx); stbi_image_free(pixels); return -1;
    }

    // ---- Preprocess: letterbox to model input size ----
    uint8_t *model_input = (uint8_t *)malloc(mw * mh * mc);
    letterbox_t lb;
    memset(&lb, 0, sizeof(lb));
    rgb_letterbox(pixels, iw, ih, model_input, mw, mh, &lb, 114);
    fprintf(stderr, "[test320] letterbox: src=%dx%d -> %dx%d scale=%.4f pad=(%d,%d)\n",
            iw, ih, mw, mh, lb.scale, lb.x_pad, lb.y_pad);
    dump_ppm("/tmp/test320_input.ppm", model_input, mw, mh);

    // ---- Set input ----
    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.type  = RKNN_TENSOR_UINT8;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.size  = mw * mh * mc;
    in.buf   = model_input;
    ret = rknn_inputs_set(ctx, 1, &in);
    if (ret < 0) { fprintf(stderr, "rknn_inputs_set fail ret=%d\n", ret); goto out; }

    // ---- Run ----
    ret = rknn_run(ctx, NULL);
    if (ret < 0) { fprintf(stderr, "rknn_run fail ret=%d\n", ret); goto out; }

    // ---- Get output ----
    {
    rknn_output output;
    memset(&output, 0, sizeof(output));
    output.index = 0;
    output.want_float = (!is_quant);
    ret = rknn_outputs_get(ctx, 1, &output, NULL);
    if (ret < 0) { fprintf(stderr, "rknn_outputs_get fail ret=%d\n", ret); goto out; }

    // ---- Dequantize output ----
    int8_t *qbuf = (int8_t *)output.buf;
    float *fbuf = (float *)malloc(n_dets * n_props * sizeof(float));

    fprintf(stderr, "[test320] raw INT8 first 20: ");
    for (int k = 0; k < 20 && k < n_dets * n_props; k++)
        fprintf(stderr, "%d ", qbuf[k]);
    fprintf(stderr, "\n");

    // Dequantize: float_val = (int8_val - zp) * scale
    float fmin = 1e9, fmax = -1e9;
    for (int i = 0; i < n_dets * n_props; i++) {
        fbuf[i] = ((float)qbuf[i] - (float)out_zp) * out_scale;
        if (fbuf[i] < fmin) fmin = fbuf[i];
        if (fbuf[i] > fmax) fmax = fbuf[i];
    }
    fprintf(stderr, "[test320] dequant range: [%.2f, %.2f]\n", fmin, fmax);

    // Show first detection dequantized
    fprintf(stderr, "[test320] det[0] dequant: x=%.2f y=%.2f w=%.2f h=%.2f "
            "obj=%.4f cls_scores=[%.4f,%.4f,%.4f,%.4f,%.4f]\n",
            fbuf[0], fbuf[1], fbuf[2], fbuf[3], fbuf[4],
            fbuf[5], fbuf[6], fbuf[7], fbuf[8], fbuf[9]);

    // ---- Decode detections ----
    struct det { float score, x1, y1, w, h; int cls; };
    std::vector<det> candidates;
    float thresh_score = 0.50f;  // higher due to quantization scale inflation

    for (int i = 0; i < n_dets; i++) {
        float *row = fbuf + i * n_props;
        // No sigmoid: quantization scale (1.516) maps [0,1] loosely.
        // obj and cls_score may be >1.0 after dequant.  Use direct values.
        float obj = row[4];
        int best_cls = 0;
        float best_cls_score = row[5];
        for (int c = 1; c < 80; c++) {
            if (row[5 + c] > best_cls_score) {
                best_cls_score = row[5 + c];
                best_cls = c;
            }
        }
        float score = obj * best_cls_score;
        if (score >= thresh_score) {
            float cx = row[0], cy = row[1], bw = row[2], bh = row[3];
            det d;
            d.score = score;
            d.cls = best_cls;
            d.x1 = cx - bw * 0.5f;  // top-left x in model space (320x320)
            d.y1 = cy - bh * 0.5f;  // top-left y in model space
            d.w  = bw;
            d.h  = bh;
            candidates.push_back(d);
        }
    }

    fprintf(stderr, "[test320] candidates above %.2f: %zu\n", thresh_score, candidates.size());

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(),
              [](const det &a, const det &b) { return a.score > b.score; });

    // Show top 5 candidates
    for (int i = 0; i < 5 && i < (int)candidates.size(); i++) {
        fprintf(stderr, "[test320]   cand[%d] cls=%d score=%.3f "
                "box_model=[%.1f,%.1f,%.1f,%.1f]\n",
                i, candidates[i].cls, candidates[i].score,
                candidates[i].x1, candidates[i].y1, candidates[i].w, candidates[i].h);
    }

    // ---- NMS per class ----
    std::vector<det> results;
    for (int c = 0; c < 80; c++) {
        // Collect candidates for this class
        std::vector<int> idx;
        for (int i = 0; i < (int)candidates.size(); i++)
            if (candidates[i].cls == c) idx.push_back(i);

        // Sort by score
        std::sort(idx.begin(), idx.end(),
                  [&](int a, int b) { return candidates[a].score > candidates[b].score; });

        std::vector<bool> keep(idx.size(), true);
        for (int i = 0; i < (int)idx.size(); i++) {
            if (!keep[i]) continue;
            det &di = candidates[idx[i]];
            float x1a = di.x1, y1a = di.y1, x2a = di.x1 + di.w, y2a = di.y1 + di.h;
            for (int j = i + 1; j < (int)idx.size(); j++) {
                if (!keep[j]) continue;
                det &dj = candidates[idx[j]];
                float x1b = dj.x1, y1b = dj.y1, x2b = dj.x1 + dj.w, y2b = dj.y1 + dj.h;
                if (CalcIoU(x1a, y1a, x2a, y2a, x1b, y1b, x2b, y2b) > NMS_THRESH)
                    keep[j] = false;
            }
        }
        for (int i = 0; i < (int)idx.size(); i++)
            if (keep[i]) results.push_back(candidates[idx[i]]);
    }

    // Sort final results by score
    std::sort(results.begin(), results.end(),
              [](const det &a, const det &b) { return a.score > b.score; });

    // ---- Print results (map to original image coords) ----
    printf("\n===== RESULTS: %zu detections =====\n", results.size());
    for (size_t i = 0; i < results.size(); i++) {
        det &d = results[i];
        // Map from model space (320x320) to original image space using letterbox
        float orig_x1 = (d.x1 - lb.x_pad) / lb.scale;
        float orig_y1 = (d.y1 - lb.y_pad) / lb.scale;
        float orig_w  = d.w / lb.scale;
        float orig_h  = d.h / lb.scale;
        // Clamp to image bounds
        if (orig_x1 < 0) orig_x1 = 0;
        if (orig_y1 < 0) orig_y1 = 0;
        if (orig_x1 + orig_w > iw) orig_w = iw - orig_x1;
        if (orig_y1 + orig_h > ih) orig_h = ih - orig_y1;

        printf("  [%zu] cls=%d score=%.3f box_orig=[%.0f,%.0f,%.0f,%.0f]\n",
               i, d.cls, d.score, orig_x1, orig_y1, orig_w, orig_h);
    }

    rknn_outputs_release(ctx, 1, &output);
    free(fbuf);
    } // end output processing block

out:
    free(model_input);
    rknn_destroy(ctx);
    stbi_image_free(pixels);
    return 0;
}
