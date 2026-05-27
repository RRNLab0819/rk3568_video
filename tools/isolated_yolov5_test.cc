// isolated_yolov5_test.cc - exact replica of official rknn_model_zoo-1.6.0 YOLOv5 flow
//
// Usage: ./isolated_yolov5_test <model.rknn> <image.jpg>
//
// This test strictly uses the official:
//   - UINT8 NHWC input (never XOR 0x80!)
//   - Bilinear letterbox (same as official image_utils CPU fallback)
//   - Official postprocess.cc logic
//
// Build: see Makefile.isolated

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <set>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

// ---- Our types (identical to official common.h) ----
typedef enum {
    IMAGE_FORMAT_GRAY8,
    IMAGE_FORMAT_RGB888,
    IMAGE_FORMAT_RGBA8888,
} image_format_t;

typedef struct {
    int width, height, width_stride, height_stride;
    image_format_t format;
    unsigned char* virt_addr;
    int size, fd;
} image_buffer_t;

typedef struct {
    int left, top, right, bottom;
} image_rect_t;

typedef struct {
    float scale;
    int x_pad, y_pad;
} letterbox_t;

// ---- rknn_app_context_t (identical to official yolov5.h) ----
#include "rknn_api.h"

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;
    int model_channel, model_width, model_height;
    bool is_quant;
} rknn_app_context_t;

// ---- YOLOv5 constants (identical to official postprocess.h) ----
#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 80
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    int id, count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

// ---- Anchor (identical to official postprocess.cc) ----
static const int anchor[3][6] = {
    {10, 13, 16, 30, 33, 23},
    {30, 61, 62, 45, 59, 119},
    {116, 90, 156, 198, 373, 326}
};

// ---- Utility functions (identical to official postprocess.cc) ----
static int clamp(float val, int min, int max) {
    return val > min ? (val < max ? val : max) : min;
}

static int32_t _clip(float val, float min, float max) {
    float f = val <= min ? min : (val >= max ? max : val);
    return f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)_clip(dst_val, -128, 127);
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0,
                               float xmin1, float ymin1, float xmax1, float ymax1) {
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) +
              (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int nms(int validCount, std::vector<float> &outputLocations,
               std::vector<int> classIds, std::vector<int> &order,
               int filterId, float threshold) {
    for (int i = 0; i < validCount; ++i) {
        if (order[i] == -1 || classIds[i] != filterId) continue;
        int n = order[i];
        for (int j = i + 1; j < validCount; ++j) {
            int m = order[j];
            if (m == -1 || classIds[i] != filterId) continue;
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];
            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];
            if (CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1) > threshold)
                order[j] = -1;
        }
    }
    return 0;
}

static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right,
                                      std::vector<int> &indices) {
    float key;
    int key_index, low = left, high = right;
    if (left < right) {
        key_index = indices[left];
        key = input[left];
        while (low < high) {
            while (low < high && input[high] <= key) high--;
            input[low] = input[high]; indices[low] = indices[high];
            while (low < high && input[low] >= key) low++;
            input[high] = input[low]; indices[high] = indices[low];
        }
        input[low] = key; indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

// ---- process_i8: identical to official postprocess.cc (rk356x path) ----
static int process_i8(int8_t *input, int *anchor, int grid_h, int grid_w,
                      int height, int width, int stride,
                      std::vector<float> &boxes, std::vector<float> &objProbs,
                      std::vector<int> &classId, float threshold,
                      int32_t zp, float scale) {
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);
    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < grid_h; i++) {
            for (int j = 0; j < grid_w; j++) {
                int8_t box_confidence = input[(PROP_BOX_SIZE * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= thres_i8) {
                    int offset = (PROP_BOX_SIZE * a) * grid_len + i * grid_w + j;
                    int8_t *in_ptr = input + offset;
                    float box_x = (deqnt_affine_to_f32(*in_ptr, zp, scale)) * 2.0 - 0.5;
                    float box_y = (deqnt_affine_to_f32(in_ptr[grid_len], zp, scale)) * 2.0 - 0.5;
                    float box_w = (deqnt_affine_to_f32(in_ptr[2 * grid_len], zp, scale)) * 2.0;
                    float box_h = (deqnt_affine_to_f32(in_ptr[3 * grid_len], zp, scale)) * 2.0;
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                    box_x -= (box_w / 2.0);
                    box_y -= (box_h / 2.0);

                    int8_t maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < OBJ_CLASS_NUM; ++k) {
                        int8_t prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs) { maxClassId = k; maxClassProbs = prob; }
                    }
                    if (maxClassProbs > thres_i8) {
                        objProbs.push_back(deqnt_affine_to_f32(maxClassProbs, zp, scale) *
                                           deqnt_affine_to_f32(box_confidence, zp, scale));
                        classId.push_back(maxClassId);
                        validCount++;
                        boxes.push_back(box_x); boxes.push_back(box_y);
                        boxes.push_back(box_w); boxes.push_back(box_h);
                    }
                }
            }
        }
    }
    return validCount;
}

// ---- process_fp32: identical to official postprocess.cc ----
static int process_fp32(float *input, int *anchor, int grid_h, int grid_w,
                        int height, int width, int stride,
                        std::vector<float> &boxes, std::vector<float> &objProbs,
                        std::vector<int> &classId, float threshold) {
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < grid_h; i++) {
            for (int j = 0; j < grid_w; j++) {
                float box_confidence = input[(PROP_BOX_SIZE * a + 4) * grid_len + i * grid_w + j];
                if (box_confidence >= threshold) {
                    int offset = (PROP_BOX_SIZE * a) * grid_len + i * grid_w + j;
                    float *in_ptr = input + offset;
                    float box_x = *in_ptr * 2.0 - 0.5;
                    float box_y = in_ptr[grid_len] * 2.0 - 0.5;
                    float box_w = in_ptr[2 * grid_len] * 2.0;
                    float box_h = in_ptr[3 * grid_len] * 2.0;
                    box_x = (box_x + j) * (float)stride;
                    box_y = (box_y + i) * (float)stride;
                    box_w = box_w * box_w * (float)anchor[a * 2];
                    box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                    box_x -= (box_w / 2.0);
                    box_y -= (box_h / 2.0);

                    float maxClassProbs = in_ptr[5 * grid_len];
                    int maxClassId = 0;
                    for (int k = 1; k < OBJ_CLASS_NUM; ++k) {
                        float prob = in_ptr[(5 + k) * grid_len];
                        if (prob > maxClassProbs) { maxClassId = k; maxClassProbs = prob; }
                    }
                    if (maxClassProbs > threshold) {
                        objProbs.push_back(maxClassProbs * box_confidence);
                        classId.push_back(maxClassId);
                        validCount++;
                        boxes.push_back(box_x); boxes.push_back(box_y);
                        boxes.push_back(box_w); boxes.push_back(box_h);
                    }
                }
            }
        }
    }
    return validCount;
}

// ---- Official post_process (rk356x path, identical to official postprocess.cc) ----
static int post_process(rknn_app_context_t *app_ctx, void *outputs,
                         letterbox_t *letter_box, float conf_threshold,
                         float nms_threshold, object_detect_result_list *od_results) {
    rknn_output *_outputs = (rknn_output *)outputs;
    std::vector<float> filterBoxes, objProbs;
    std::vector<int> classId;
    int validCount = 0, stride = 0, grid_h = 0, grid_w = 0;
    int model_in_w = app_ctx->model_width, model_in_h = app_ctx->model_height;
    memset(od_results, 0, sizeof(object_detect_result_list));

    for (int i = 0; i < 3; i++) {
        grid_h = app_ctx->output_attrs[i].dims[2];
        grid_w = app_ctx->output_attrs[i].dims[3];
        stride = model_in_h / grid_h;
        if (app_ctx->is_quant) {
            validCount += process_i8((int8_t *)_outputs[i].buf, (int *)anchor[i],
                                      grid_h, grid_w, model_in_h, model_in_w, stride,
                                      filterBoxes, objProbs, classId, conf_threshold,
                                      app_ctx->output_attrs[i].zp,
                                      app_ctx->output_attrs[i].scale);
        } else {
            validCount += process_fp32((float *)_outputs[i].buf, (int *)anchor[i],
                                        grid_h, grid_w, model_in_h, model_in_w, stride,
                                        filterBoxes, objProbs, classId, conf_threshold);
        }
    }

    if (validCount <= 0) return 0;

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i) indexArray.push_back(i);
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    for (auto c : class_set)
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);

    int last_count = 0;
    od_results->count = 0;
    for (int i = 0; i < validCount; ++i) {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE) continue;
        int n = indexArray[i];
        float x1 = filterBoxes[n * 4 + 0] - letter_box->x_pad;
        float y1 = filterBoxes[n * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[i];
        od_results->results[last_count].box.left   = (int)(clamp(x1, 0, model_in_w) / letter_box->scale);
        od_results->results[last_count].box.top    = (int)(clamp(y1, 0, model_in_h) / letter_box->scale);
        od_results->results[last_count].box.right  = (int)(clamp(x2, 0, model_in_w) / letter_box->scale);
        od_results->results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) / letter_box->scale);
        od_results->results[last_count].prop  = obj_conf;
        od_results->results[last_count].cls_id = id;
        last_count++;
    }
    od_results->count = last_count;
    return 0;
}

// ---- Bilinear RGB letterbox (exact same logic as official image_utils CPU fallback) ----
static void rgb_letterbox(const uint8_t *src, int sw, int sh,
                          uint8_t *dst, int dw, int dh,
                          letterbox_t *lb, int bg_color) {
    float scale_w = (float)dw / sw, scale_h = (float)dh / sh;
    float scale = (scale_w < scale_h) ? scale_w : scale_h;
    int rw = (int)(sw * scale), rh = (int)(sh * scale);
    if (rw % 4 != 0) rw -= rw % 4;
    if (rh % 2 != 0) rh -= rh % 2;
    int x_pad = (dw - rw) / 2, y_pad = (dh - rh) / 2;
    if (x_pad % 2 != 0) { x_pad -= x_pad % 2; if (x_pad < 0) x_pad = 0; }
    if (y_pad % 2 != 0) { y_pad -= y_pad % 2; if (y_pad < 0) y_pad = 0; }

    lb->scale = scale; lb->x_pad = x_pad; lb->y_pad = y_pad;
    memset(dst, bg_color, dw * dh * 3);

    float x_ratio = (float)sw / rw, y_ratio = (float)sh / rh;
    for (int dy = 0; dy < rh; dy++) {
        float sy_f = dy * y_ratio;
        int sy = (int)sy_f;
        float y_diff = sy_f - sy;
        if (sy >= sh - 1) { sy = sh - 2; y_diff = 1.0f; }
        int src_row0 = sy * sw * 3, src_row1 = (sy + 1) * sw * 3;
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
                float v = src[s00+c] * (1-x_diff) * (1-y_diff) +
                          src[s01+c] * x_diff * (1-y_diff) +
                          src[s10+c] * (1-x_diff) * y_diff +
                          src[s11+c] * x_diff * y_diff;
                dst[d+c] = (uint8_t)(v + 0.5f);
            }
        }
    }
}

// ---- Dump helpers ----
static void dump_tensor_attr(rknn_tensor_attr *a) {
    fprintf(stderr, "  [%d] name=%s n_dims=%d dims=[%d,%d,%d,%d] n_elems=%d size=%d "
            "fmt=%d type=%d qnt=%d zp=%d scale=%f\n",
            a->index, a->name, a->n_dims,
            a->dims[0], a->dims[1], a->dims[2], a->dims[3],
            a->n_elems, a->size, a->fmt, a->type, a->qnt_type,
            a->zp, a->scale);
}

static void dump_ppm(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, w * h * 3, fp);
    fclose(fp);
    fprintf(stderr, "[test] wrote %s (%dx%d)\n", path, w, h);
}

// =====================================================================
// init_yolov5_model - EXACT copy of official yolov5.cc
// =====================================================================
static int init_yolov5_model(const char *model_path, rknn_app_context_t *app_ctx) {
    int ret, model_len = 0;
    char *model;
    rknn_context ctx = 0;

    // Load model
    FILE *f = fopen(model_path, "rb");
    if (!f) { fprintf(stderr, "[test] cannot open %s\n", model_path); return -1; }
    fseek(f, 0, SEEK_END);
    model_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    model = (char *)malloc(model_len);
    fread(model, 1, model_len, f);
    fclose(f);

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) { fprintf(stderr, "[test] rknn_init fail ret=%d\n", ret); return -1; }

    // Get I/O num
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) { fprintf(stderr, "[test] query IO_NUM fail\n"); return -1; }
    fprintf(stderr, "[test] input num=%d, output num=%d\n", io_num.n_input, io_num.n_output);

    // Get input attrs
    fprintf(stderr, "[test] --- input tensors ---\n");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) return -1;
        dump_tensor_attr(&input_attrs[i]);
    }

    // Get output attrs
    fprintf(stderr, "[test] --- output tensors ---\n");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) return -1;
        dump_tensor_attr(&output_attrs[i]);
    }

    // Set context
    app_ctx->rknn_ctx = ctx;
    if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
        output_attrs[0].type != RKNN_TENSOR_FLOAT16) {
        app_ctx->is_quant = true;
    } else {
        app_ctx->is_quant = false;
    }
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height  = input_attrs[0].dims[2];
        app_ctx->model_width   = input_attrs[0].dims[3];
    } else {
        app_ctx->model_height  = input_attrs[0].dims[1];
        app_ctx->model_width   = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    fprintf(stderr, "[test] model: %dx%dx%d is_quant=%d\n",
            app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel, app_ctx->is_quant);
    return 0;
}

static void release_yolov5_model(rknn_app_context_t *app_ctx) {
    if (app_ctx->rknn_ctx) { rknn_destroy(app_ctx->rknn_ctx); app_ctx->rknn_ctx = 0; }
    free(app_ctx->input_attrs);
    free(app_ctx->output_attrs);
}

// =====================================================================
// inference_yolov5_model - EXACT copy of official yolov5.cc inference
// Uses UINT8 NHWC input (never XOR 0x80!)
// =====================================================================
static int inference_yolov5_model(rknn_app_context_t *app_ctx,
                                   image_buffer_t *img,
                                   object_detect_result_list *od_results) {
    int ret;
    image_buffer_t dst_img;
    letterbox_t letter_box;
    rknn_input inputs[app_ctx->io_num.n_input];
    rknn_output outputs[app_ctx->io_num.n_output];
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    int bg_color = 114;

    memset(od_results, 0, sizeof(*od_results));
    memset(&letter_box, 0, sizeof(letterbox_t));
    memset(&dst_img, 0, sizeof(image_buffer_t));
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    // Pre Process: allocate dst buffer
    dst_img.width  = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size   = dst_img.width * dst_img.height * 3;
    dst_img.virt_addr = (unsigned char *)malloc(dst_img.size);
    if (!dst_img.virt_addr) { fprintf(stderr, "[test] malloc fail\n"); return -1; }

    // Letterbox (CPU bilinear - same as official CPU fallback)
    rgb_letterbox(img->virt_addr, img->width, img->height,
                  dst_img.virt_addr, dst_img.width, dst_img.height,
                  &letter_box, bg_color);

    fprintf(stderr, "[test] letterbox: src=%dx%d dst=%dx%d scale=%.4f x_pad=%d y_pad=%d\n",
            img->width, img->height, dst_img.width, dst_img.height,
            letter_box.scale, letter_box.x_pad, letter_box.y_pad);

    // Dump model input for verification
    dump_ppm("/tmp/isolated_test_input.ppm", dst_img.virt_addr,
             dst_img.width, dst_img.height);

    // Set Input Data - UINT8 NHWC (SAME AS OFFICIAL!)
    inputs[0].index = 0;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;
    inputs[0].size  = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf   = dst_img.virt_addr;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0) { fprintf(stderr, "[test] rknn_inputs_set fail ret=%d\n", ret); goto out; }

    // Run
    ret = rknn_run(app_ctx->rknn_ctx, NULL);
    if (ret < 0) { fprintf(stderr, "[test] rknn_run fail ret=%d\n", ret); goto out; }

    // Get Output
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = (!app_ctx->is_quant);
    }
    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0) { fprintf(stderr, "[test] rknn_outputs_get fail ret=%d\n", ret); goto out; }

    // Post Process
    post_process(app_ctx, outputs, &letter_box, box_conf_threshold, nms_threshold, od_results);

    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);

out:
    free(dst_img.virt_addr);
    return ret;
}

// =====================================================================
// Main
// =====================================================================
int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <model.rknn> <image.jpg>\n", argv[0]);
        return -1;
    }
    const char *model_path = argv[1], *image_path = argv[2];

    // Read JPEG using stb_image (RGB format - same as official read_image_stb)
    int iw, ih, ic;
    unsigned char *pixels = stbi_load(image_path, &iw, &ih, &ic, 3);  // force 3-channel RGB
    if (!pixels) { fprintf(stderr, "[test] stbi_load fail: %s\n", image_path); return -1; }
    fprintf(stderr, "[test] loaded %s: %dx%d channels=%d\n", image_path, iw, ih, ic);

    image_buffer_t src_image;
    src_image.width  = iw;
    src_image.height = ih;
    src_image.format = IMAGE_FORMAT_RGB888;
    src_image.virt_addr = pixels;
    src_image.size = iw * ih * 3;

    // Dump original for comparison
    dump_ppm("/tmp/isolated_test_orig.ppm", pixels, iw, ih);

    // Init model
    rknn_app_context_t app_ctx;
    memset(&app_ctx, 0, sizeof(app_ctx));
    int ret = init_yolov5_model(model_path, &app_ctx);
    if (ret != 0) { fprintf(stderr, "[test] init model fail\n"); stbi_image_free(pixels); return -1; }

    // Run inference
    object_detect_result_list od_results;
    ret = inference_yolov5_model(&app_ctx, &src_image, &od_results);
    if (ret != 0) { fprintf(stderr, "[test] inference fail\n"); }

    // Print results
    printf("\n===== RESULTS: %d detections =====\n", od_results.count);
    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *d = &od_results.results[i];
        printf("  [%d] cls=%d conf=%.3f box=[%d,%d,%d,%d] (orig image coords)\n",
               i, d->cls_id, d->prop,
               d->box.left, d->box.top, d->box.right, d->box.bottom);
    }

    // Cleanup
    release_yolov5_model(&app_ctx);
    stbi_image_free(pixels);
    return 0;
}
