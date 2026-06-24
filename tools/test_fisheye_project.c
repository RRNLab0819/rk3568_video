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

    view.yaw_deg = 25.0f;
    assert(fisheye_view_to_raw_uv(&cam, &view, 0.5f, 0.5f, &u, &v));
    assert(u > 0.5f);

    return 0;
}
