/* src/fisheye_project.h
 * Shared fisheye <-> virtual pinhole view projection helpers.
 */
#ifndef FISHEYE_PROJECT_H
#define FISHEYE_PROJECT_H

#include "fisheye_mesh.h"
#include <stdbool.h>

typedef struct {
    float fov_h_deg;
    float yaw_deg;
    float pitch_deg;
    int   out_w;
    int   out_h;
    int   rotate_deg;
    bool  flip_x;
    bool  flip_y;
} fisheye_view_t;

bool fisheye_view_to_raw_uv(const fisheye_cam_t *cam,
                            const fisheye_view_t *view,
                            float out_u, float out_v,
                            float *raw_u, float *raw_v);

bool fisheye_raw_pixel_to_view_uv(const fisheye_cam_t *cam,
                                  const fisheye_view_t *view,
                                  float raw_x, float raw_y,
                                  float *out_u, float *out_v);

bool fisheye_raw_pixel_to_camera_ray(const fisheye_cam_t *cam,
                                     const fisheye_view_t *view,
                                     float raw_x, float raw_y,
                                     float *ray_x, float *ray_y, float *ray_z);

float fisheye_estimate_distance_from_bbox_height(const fisheye_cam_t *cam,
                                                 const fisheye_view_t *view,
                                                 float raw_x, float raw_y,
                                                 float raw_w, float raw_h,
                                                 float person_height_m);

bool fisheye_project_bbox_to_view(const fisheye_cam_t *cam,
                                  const fisheye_view_t *view,
                                  float raw_x, float raw_y,
                                  float raw_w, float raw_h,
                                  float min_visible_fraction,
                                  float *out_u0, float *out_v0,
                                  float *out_u1, float *out_v1,
                                  float *visible_fraction);

#endif
