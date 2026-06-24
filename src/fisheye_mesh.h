/* src/fisheye_mesh.h
 * Fisheye undistort mesh generator — Kannala-Brandt model + per-camera params.
 */
#ifndef FISHEYE_MESH_H
#define FISHEYE_MESH_H

#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Per-camera calibration. The current mesh uses one focal value, so YAML
 * OpenCV fx/fy values are averaged when loaded at runtime. */
typedef struct {
    float cx, cy;       /* optical center in pixels */
    float focal;        /* focal length in pixels (Kannala-Brandt model) */
    float k[4];         /* distortion coefficients k1..k4 */
    int   src_w, src_h; /* source image size */
} fisheye_cam_t;

/* Detailed UV coverage statistics */
typedef struct {
    float u_min, u_max, v_min, v_max;    /* UV range */
    float valid_pct;                      /* % vertices with UV in [0,1] */
    float oob_pct;                        /* % vertices out of bounds */
    float edge_top_pct, edge_bot_pct;     /* % OOB on top/bottom edges */
    float edge_left_pct, edge_right_pct;  /* % OOB on left/right edges */
    float near_edge_pct;                  /* % within 5% of image edge */
    int   total_verts;
} fisheye_uv_stats_t;

/* Generated mesh for one camera */
typedef struct {
    int    num_verts;
    int    num_indices;
    GLuint vbo_pos;     /* vec2 screen-space positions */
    GLuint vbo_tex;     /* vec2 source-image UVs */
    GLuint ibo;         /* unsigned short triangle indices */
} fisheye_mesh_t;

/* Kannala-Brandt fisheye model */
float lens_6028_radius(float angle_rad, const fisheye_cam_t *cam);

/* Generate undistort mesh with rotate/flip support + UV statistics.
 * rotate_deg: 0/90/180/270
 * flip_x, flip_y: apply horizontal/vertical flip to source UVs
 * stats: if non-NULL, filled with detailed UV coverage stats
 * Returns 0 on success, -1 on error. */
int  fisheye_mesh_build_ex(fisheye_mesh_t *mesh,
                           const fisheye_cam_t *cam,
                           float tile_x0, float tile_y0,
                           float tile_w,  float tile_h,
                           int out_w, int out_h,
                           float fov_h,
                           float yaw_deg, float pitch_deg,
                           int rotate_deg, bool flip_x, bool flip_y,
                           fisheye_uv_stats_t *stats);

/* Backward-compat wrapper */
static inline int fisheye_mesh_build(fisheye_mesh_t *mesh,
                                     const fisheye_cam_t *cam,
                                     float x0, float y0, float w, float h,
                                     int ow, int oh, float fov)
{
    return fisheye_mesh_build_ex(mesh, cam, x0, y0, w, h,
                                 ow, oh, fov, 0.0f, 0.0f,
                                 0, false, false, NULL);
}

void fisheye_mesh_destroy(fisheye_mesh_t *mesh);

/* Save a color-coded UV coverage debug PPM.
 * Green = valid UV [0,1], Red = OOB, Blue = near edge.
 * The PPM is (out_w x out_h) and maps grid vertex UVs. */
void fisheye_mesh_dump_uv_debug(const char *path,
                                const fisheye_uv_stats_t *stats,
                                const fisheye_cam_t *cam,
                                float fov_h, int out_w, int out_h,
                                int rotate_deg, bool flip_x, bool flip_y);

/* Built-in fallback camera calibrations, optionally overridden at startup. */
extern fisheye_cam_t g_fisheye_cams[4];

/* Load calib_videoN.yaml files from a directory and override g_fisheye_cams.
 * The YAML parser is intentionally small and supports the OpenCV output used
 * by tools/calibrate_chess.py. Returns the number of cameras loaded. */
int fisheye_load_calibration_dir(const char *dir, int n_cams);

#endif
