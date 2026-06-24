#include "fisheye_project.h"
#include <assert.h>
#include <math.h>

float lens_6028_radius(float angle_rad, const fisheye_cam_t *cam)
{
    float t2 = angle_rad * angle_rad;
    float t4 = t2 * t2;
    float t6 = t4 * t2;
    float t8 = t4 * t4;
    float td = angle_rad * (1.0f + cam->k[0]*t2 + cam->k[1]*t4 +
                            cam->k[2]*t6 + cam->k[3]*t8);
    return cam->focal * td;
}

static int closef(float a, float b)
{
    return fabsf(a - b) < 0.001f;
}

int main(void)
{
    fisheye_cam_t cam = {
        .cx = 960.0f, .cy = 540.0f, .focal = 540.0f,
        .k = {0.02f, 0.001f, 0.0f, 0.0f},
        .src_w = 1920, .src_h = 1080,
    };
    fisheye_view_t view = {
        .fov_h_deg = 120.0f,
        .yaw_deg = 0.0f,
        .pitch_deg = 0.0f,
        .out_w = 960,
        .out_h = 540,
        .rotate_deg = 0,
        .flip_x = false,
        .flip_y = false,
    };
    float u = 0.0f, v = 0.0f;

    assert(fisheye_view_to_raw_uv(&cam, &view, 0.5f, 0.5f, &u, &v));
    assert(closef(u, 0.5f));
    assert(closef(v, 0.5f));

    assert(fisheye_raw_pixel_to_view_uv(&cam, &view, 960.0f, 540.0f, &u, &v));
    assert(closef(u, 0.5f));
    assert(closef(v, 0.5f));

    float dist = fisheye_estimate_distance_from_bbox_height(&cam, &view,
                                                           910.0f, 450.0f,
                                                           100.0f, 180.0f,
                                                           1.70f);
    assert(dist > 4.0f && dist < 8.0f);

    float u0, v0, u1, v1, vis;
    assert(fisheye_project_bbox_to_view(&cam, &view,
                                        900.0f, 480.0f, 140.0f, 160.0f,
                                        0.25f, &u0, &v0, &u1, &v1, &vis));
    assert(u0 >= 0.0f && u1 <= 1.0f);
    assert(v0 >= 0.0f && v1 <= 1.0f);
    assert(u1 > u0);
    assert(v1 > v0);

    assert(!fisheye_project_bbox_to_view(&cam, &view,
                                         1880.0f, 20.0f, 60.0f, 80.0f,
                                         0.50f, &u0, &v0, &u1, &v1, &vis));

    view.yaw_deg = 25.0f;
    assert(fisheye_view_to_raw_uv(&cam, &view, 0.5f, 0.5f, &u, &v));
    assert(u > 0.5f);

    return 0;
}
