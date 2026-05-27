#ifndef _RKNN_YOLOV5_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV5_DEMO_POSTPROCESS_H_

#include <stdint.h>
#ifdef __cplusplus
#include <vector>
#endif
#include "rknn_api.h"
#include "rknn_common.h"  /* letterbox_t defined here */

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 80
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25
#define PROP_BOX_SIZE (5 + OBJ_CLASS_NUM)

// class rknn_app_context_t;

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    int id;
    int count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

int init_post_process();
void deinit_post_process();
char *coco_cls_to_name(int cls_id);
int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box, float conf_threshold, float nms_threshold, object_detect_result_list *od_results);

void deinitPostProcess();

#ifdef __cplusplus
extern "C" {
#endif

int post_process_yolov5(void *ctx, void *outputs, void *letter_box,
                         float conf, float nms,
                         int *out_class, float *out_conf,
                         float *out_x, float *out_y, float *out_w, float *out_h,
                         int max_dets);

#ifdef __cplusplus
}
#endif

#endif //_RKNN_YOLOV5_DEMO_POSTPROCESS_H_
