/* src/fisheye_project.c */
#include "fisheye_project.h"
#include <math.h>

static void apply_rotate_flip_uv(float *u, float *v,
                                 int rotate_deg, bool flip_x, bool flip_y)
{
    float tu = *u - 0.5f;
    float tv = *v - 0.5f;
    float ru, rv;

    switch (rotate_deg) {
    case 90:  ru = -tv; rv =  tu; break;
    case 180: ru = -tu; rv = -tv; break;
    case 270: ru =  tv; rv = -tu; break;
    default:  ru =  tu; rv =  tv; break;
    }
    if (flip_x) ru = -ru;
    if (flip_y) rv = -rv;
    *u = ru + 0.5f;
    *v = rv + 0.5f;
}

static void inverse_rotate_flip_uv(float *u, float *v,
                                   int rotate_deg, bool flip_x, bool flip_y)
{
    float ru = *u - 0.5f;
    float rv = *v - 0.5f;
    float tu, tv;

    if (flip_x) ru = -ru;
    if (flip_y) rv = -rv;
    switch (rotate_deg) {
    case 90:  tu =  rv; tv = -ru; break;
    case 180: tu = -ru; tv = -rv; break;
    case 270: tu = -rv; tv =  ru; break;
    default:  tu =  ru; tv =  rv; break;
    }
    *u = tu + 0.5f;
    *v = tv + 0.5f;
}

static void view_to_camera_ray(const fisheye_view_t *view,
                               float u, float v,
                               float *rx, float *ry, float *rz)
{
    float fov_h = view->fov_h_deg * (float)(M_PI / 180.0);
    float fov_v = fov_h * (float)view->out_h / (float)view->out_w;
    float theta = (u - 0.5f) * fov_h;
    float phi = (v - 0.5f) * fov_v;
    float x = tanf(theta);
    float y = tanf(phi) / cosf(theta);
    float z = 1.0f;

    float yaw = view->yaw_deg * (float)(M_PI / 180.0);
    float pitch = view->pitch_deg * (float)(M_PI / 180.0);
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);

    float x1 = cy * x + sy * z;
    float y1 = y;
    float z1 = -sy * x + cy * z;

    *rx = x1;
    *ry = cp * y1 - sp * z1;
    *rz = sp * y1 + cp * z1;
}

static void camera_to_view_ray(const fisheye_view_t *view,
                               float cx, float cy, float cz,
                               float *vx, float *vy, float *vz)
{
    float yaw = view->yaw_deg * (float)(M_PI / 180.0);
    float pitch = view->pitch_deg * (float)(M_PI / 180.0);
    float cyaw = cosf(yaw), syaw = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);

    float x1 = cx;
    float y1 = cp * cy + sp * cz;
    float z1 = -sp * cy + cp * cz;

    *vx = cyaw * x1 - syaw * z1;
    *vy = y1;
    *vz = syaw * x1 + cyaw * z1;
}

static float invert_kb_radius(const fisheye_cam_t *cam, float r_real)
{
    float inc = r_real / cam->focal;
    for (int iter = 0; iter < 8; iter++) {
        float t2 = inc * inc;
        float t4 = t2 * t2;
        float t6 = t4 * t2;
        float t8 = t4 * t4;
        float poly = 1.0f + cam->k[0]*t2 + cam->k[1]*t4 +
                     cam->k[2]*t6 + cam->k[3]*t8;
        float f = cam->focal * inc * poly - r_real;
        float deriv = cam->focal * (1.0f + 3.0f*cam->k[0]*t2 +
                      5.0f*cam->k[1]*t4 + 7.0f*cam->k[2]*t6 +
                      9.0f*cam->k[3]*t8);
        if (fabsf(deriv) < 1e-6f) break;
        inc -= f / deriv;
        if (inc < 0.0f) inc = 0.0f;
    }
    return inc;
}

bool fisheye_view_to_raw_uv(const fisheye_cam_t *cam,
                            const fisheye_view_t *view,
                            float out_u, float out_v,
                            float *raw_u, float *raw_v)
{
    float rx, ry, rz;
    view_to_camera_ray(view, out_u, out_v, &rx, &ry, &rz);

    float r_xy = sqrtf(rx * rx + ry * ry);
    float inc = atan2f(r_xy, rz);
    if (inc < 0.0f) inc = -inc;
    float r_real = lens_6028_radius(inc, cam);

    float sx = cam->cx;
    float sy = cam->cy;
    if (r_xy > 1e-6f) {
        sx += rx * (r_real / r_xy);
        sy += ry * (r_real / r_xy);
    }

    *raw_u = sx / (float)cam->src_w;
    *raw_v = sy / (float)cam->src_h;
    apply_rotate_flip_uv(raw_u, raw_v, view->rotate_deg, view->flip_x, view->flip_y);
    return (*raw_u >= -0.25f && *raw_u <= 1.25f &&
            *raw_v >= -0.25f && *raw_v <= 1.25f);
}

bool fisheye_raw_pixel_to_view_uv(const fisheye_cam_t *cam,
                                  const fisheye_view_t *view,
                                  float raw_x, float raw_y,
                                  float *out_u, float *out_v)
{
    float rx, ry, rz;
    if (!fisheye_raw_pixel_to_camera_ray(cam, view, raw_x, raw_y, &rx, &ry, &rz))
        return false;

    float vx, vy, vz;
    camera_to_view_ray(view, rx, ry, rz, &vx, &vy, &vz);
    if (vz <= 1e-5f) return false;

    float theta = atan2f(vx, vz);
    float phi = atan2f(vy * cosf(theta), vz);
    float fov_h = view->fov_h_deg * (float)(M_PI / 180.0);
    float fov_v = fov_h * (float)view->out_h / (float)view->out_w;
    *out_u = theta / fov_h + 0.5f;
    *out_v = phi / fov_v + 0.5f;
    return (*out_u >= -0.05f && *out_u <= 1.05f &&
            *out_v >= -0.05f && *out_v <= 1.05f);
}

bool fisheye_raw_pixel_to_camera_ray(const fisheye_cam_t *cam,
                                     const fisheye_view_t *view,
                                     float raw_x, float raw_y,
                                     float *ray_x, float *ray_y, float *ray_z)
{
    float src_u = raw_x / (float)cam->src_w;
    float src_v = raw_y / (float)cam->src_h;
    if (src_u < -0.05f || src_u > 1.05f || src_v < -0.05f || src_v > 1.05f)
        return false;

    inverse_rotate_flip_uv(&src_u, &src_v, view->rotate_deg, view->flip_x, view->flip_y);
    float dx = src_u * (float)cam->src_w - cam->cx;
    float dy = src_v * (float)cam->src_h - cam->cy;
    float r_real = sqrtf(dx * dx + dy * dy);

    float rx = 0.0f, ry = 0.0f, rz = 1.0f;
    if (r_real > 1e-5f) {
        float inc = invert_kb_radius(cam, r_real);
        float s = sinf(inc) / r_real;
        rx = dx * s;
        ry = dy * s;
        rz = cosf(inc);
    }

    if (ray_x) *ray_x = rx;
    if (ray_y) *ray_y = ry;
    if (ray_z) *ray_z = rz;
    return true;
}

float fisheye_estimate_distance_from_bbox_height(const fisheye_cam_t *cam,
                                                 const fisheye_view_t *view,
                                                 float raw_x, float raw_y,
                                                 float raw_w, float raw_h,
                                                 float person_height_m)
{
    float tx, ty, tz, bx, by, bz;
    float cx = raw_x + raw_w * 0.5f;
    if (!cam || !view || raw_h <= 2.0f || person_height_m <= 0.2f)
        return 0.0f;
    if (!fisheye_raw_pixel_to_camera_ray(cam, view, cx, raw_y, &tx, &ty, &tz))
        return 0.0f;
    if (!fisheye_raw_pixel_to_camera_ray(cam, view, cx, raw_y + raw_h, &bx, &by, &bz))
        return 0.0f;

    float dot = tx * bx + ty * by + tz * bz;
    if (dot < -1.0f) dot = -1.0f;
    if (dot > 1.0f) dot = 1.0f;
    float angle = acosf(dot);
    if (angle < 0.001f)
        return 0.0f;

    return person_height_m / (2.0f * tanf(angle * 0.5f));
}

static float clamp01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

bool fisheye_project_bbox_to_view(const fisheye_cam_t *cam,
                                  const fisheye_view_t *view,
                                  float raw_x, float raw_y,
                                  float raw_w, float raw_h,
                                  float min_visible_fraction,
                                  float *out_u0, float *out_v0,
                                  float *out_u1, float *out_v1,
                                  float *visible_fraction)
{
    const int steps = 12;
    int total = 0;
    int visible = 0;
    float min_u = 1.0f, min_v = 1.0f, max_u = 0.0f, max_v = 0.0f;

    if (!cam || !view || raw_w <= 1.0f || raw_h <= 1.0f)
        return false;

    for (int edge = 0; edge < 4; edge++) {
        for (int s = 0; s <= steps; s++) {
            float t = (float)s / (float)steps;
            float px, py;
            float u, v;
            total++;

            if (edge == 0) {
                px = raw_x + raw_w * t; py = raw_y;
            } else if (edge == 1) {
                px = raw_x + raw_w; py = raw_y + raw_h * t;
            } else if (edge == 2) {
                px = raw_x + raw_w * (1.0f - t); py = raw_y + raw_h;
            } else {
                px = raw_x; py = raw_y + raw_h * (1.0f - t);
            }

            if (!fisheye_raw_pixel_to_view_uv(cam, view, px, py, &u, &v))
                continue;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
                continue;

            u = clamp01(u);
            v = clamp01(v);
            if (u < min_u) min_u = u;
            if (u > max_u) max_u = u;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
            visible++;
        }
    }

    float frac = total > 0 ? (float)visible / (float)total : 0.0f;
    if (visible_fraction) *visible_fraction = frac;
    if (visible < 4 || frac < min_visible_fraction)
        return false;
    if ((max_u - min_u) < 0.006f || (max_v - min_v) < 0.006f)
        return false;

    if (out_u0) *out_u0 = min_u;
    if (out_v0) *out_v0 = min_v;
    if (out_u1) *out_u1 = max_u;
    if (out_v1) *out_v1 = max_v;
    return true;
}
