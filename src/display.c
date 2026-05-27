/* src/display.c - Wayland/EGL/GLES2 display with EGL_EXT_image_dma_buf_import
 *
 * Zero-copy NV12 display: imports dma_buf fd directly as EGLImage → GL texture.
 * No memcpy of frame data.  Requires EGL_EXT_image_dma_buf_import and
 * GL_OES_EGL_image extensions (available on RK3568 Mali G52).
 *
 * Layout: 2x2 quad grid (up to 4 cameras), rendered via YUV shader.
 */
#include "display.h"
#include "shader_yuv.h"
#include "xdg-shell-client.h"
#include "fisheye_mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */

typedef enum {
    DISPLAY_MODE_GRID,           /* original 2x2 quad */
    DISPLAY_MODE_FISHEYE_GRID,   /* 2x2 fisheye mesh per tile */
    DISPLAY_MODE_AVM,            /* automotive surround-view layout */
} display_mode_t;

/* ------------------------------------------------------------------ */

static const char osd_vert[] =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "void main(){ gl_Position=vec4(a_pos,0.0,1.0); }\n";
static const char osd_frag[] =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main(){ gl_FragColor=u_color; }\n";

/* ------------------------------------------------------------------ */
/* Internal structure                                                  */
/* ------------------------------------------------------------------ */
struct display_s {
    int w, h, n_cams;
    struct wl_display      *display;
    struct wl_surface      *surface;
    struct xdg_surface     *xdg_surface;
    struct xdg_toplevel    *xdg_toplevel;
    struct wl_egl_window   *egl_window;
    EGLDisplay              egl_dpy;
    EGLContext              egl_ctx;
    EGLSurface              egl_surf;
    GLuint                  program;
    GLuint                  loc_pos;
    GLuint                  loc_tex;
    GLuint                  texY[4];
    GLuint                  texUV[4];
    bool                    has_frame[4];
    bool                    configured;

    /* Display mode (AVM_MODE > FISHEYE_MODE > grid baseline) */
    display_mode_t mode;

    /* Mesh shader (shared by FISHEYE_GRID and AVM modes) */
    GLuint      mesh_prog;
    GLint       mesh_uloc_y, mesh_uloc_uv;  /* cached uniform locations */
    GLint       mesh_aloc_pos, mesh_aloc_tex; /* cached attribute locations */

    /* Fisheye-grid mesh VBOs (2x2 tile) */
    GLuint      grid_vbo_pos[4];
    GLuint      grid_vbo_tex[4];
    GLuint      grid_ibo[4];
    int         grid_nidx[4];

    /* AVM-layout mesh VBOs (around vehicle) */
    GLuint      avm_vbo_pos[4];
    GLuint      avm_vbo_tex[4];
    GLuint      avm_ibo[4];
    int         avm_nidx[4];

    /* OSD detection overlay */
    GLuint        osd_prog;
    GLint         osd_pos, osd_color_loc;
    detection_t   dets[4][MAX_DETECTIONS];
    int           det_count[4];
    pthread_mutex_t det_lock;
};

/* Global Wayland globals (shared across all instances if needed) */
static struct wl_compositor *g_compositor;
static struct xdg_wm_base   *g_wm_base;

/* ------------------------------------------------------------------ */
/* Wayland listeners                                                   */
/* ------------------------------------------------------------------ */

static void xdg_surface_configure(void *d, struct xdg_surface *s,
                                   uint32_t serial)
{
    xdg_surface_ack_configure(s, serial);
    display_t *disp = (display_t *)d;
    disp->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *d, struct xdg_toplevel *tl,
                                    int32_t w, int32_t h,
                                    struct wl_array *states)
{
    (void)tl; (void)states;
    display_t *disp = (display_t *)d;
    if (w > 0 && h > 0 && (w != disp->w || h != disp->h)) {
        disp->w = w;
        disp->h = h;
        if (disp->egl_window)
            wl_egl_window_resize(disp->egl_window, w, h, 0, 0);
    }
}

static void xdg_toplevel_close(void *d, struct xdg_toplevel *tl)
{
    (void)d; (void)tl;
    printf("[display] toplevel close requested\n");
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

static void registry_handler(void *d, struct wl_registry *reg,
                              uint32_t id, const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = (struct wl_compositor *)wl_registry_bind(
            reg, id, &wl_compositor_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0)
        g_wm_base = (struct xdg_wm_base *)wl_registry_bind(
            reg, id, &xdg_wm_base_interface, 1);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler,
};

/* ------------------------------------------------------------------ */
/* GL shader helper                                                    */
/* ------------------------------------------------------------------ */
static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, 256, NULL, log);
        fprintf(stderr, "[display] shader error: %s\n", log);
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* Zero-copy dma_buf → EGLImage → GL texture import                   */
/* ------------------------------------------------------------------ */

/*
 * Import a single NV12 frame from a dma_buf fd.
 * Creates two EGLImages (Y + UV) from the same FD using plane offsets,
 * binds them to the pre-allocated GL textures for cam_idx, then destroys
 * the EGLImage wrappers (the GL textures retain the data).
 *
 * The fd is NOT closed here; the caller (capture / pipeline) owns it.
 */
static void import_nv12(display_t *d, int cam, const frame_t *f)
{
    if (cam < 0 || cam >= d->n_cams || !f->ptr) return;

    int ysz  = f->width * f->height;
    int uvsz = ysz / 2;

    /* Copy Y plane — strip stride padding */
    uint8_t *y  = malloc(ysz);
    uint8_t *s  = f->ptr;
    uint8_t *dptr = y;
    for (int r = 0; r < f->height; r++) {
        memcpy(dptr, s, f->width);
        s += f->stride; dptr += f->width;
    }

    /* Copy UV plane — strip stride padding */
    uint8_t *uv = malloc(uvsz);
    s = (uint8_t*)f->ptr + f->stride * f->height;
    dptr = uv;
    for (int r = 0; r < f->height / 2; r++) {
        memcpy(dptr, s, f->width);
        s += f->stride; dptr += f->width;
    }

    /* Upload to GL textures */
    glBindTexture(GL_TEXTURE_2D, d->texY[cam]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 f->width, f->height, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, d->texUV[cam]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                 f->width / 2, f->height / 2, 0,
                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    free(y); free(uv);
    d->has_frame[cam] = true;
}

/* ------------------------------------------------------------------ */
/* GLES draw helpers (use osd_prog for flat-color primitives)          */
/* ------------------------------------------------------------------ */

/* Draw a filled rectangle in NDC space */
static void draw_filled_rect(display_t *d,
                              float x0, float y0, float x1, float y1,
                              float r, float g, float b, float a)
{
    float v[] = { x0,y0, x1,y0, x1,y1, x0,y1 };
    glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(d->osd_pos);
    glUniform4f(d->osd_color_loc, r, g, b, a);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(d->osd_pos);
}

/* Draw a line-loop outline rectangle */
static void draw_outline_rect(display_t *d,
                               float x0, float y0, float x1, float y1,
                               float r, float g, float b, float a)
{
    float v[] = { x0,y0, x1,y0, x1,y1, x0,y1 };
    glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(d->osd_pos);
    glUniform4f(d->osd_color_loc, r, g, b, a);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    glDisableVertexAttribArray(d->osd_pos);
}

/* Simplified vehicle top-view placeholder: dark body + lighter roof + windshield bar */
static void draw_vehicle_placeholder(display_t *d, float cx, float cy,
                                      float body_w, float body_h)
{
    float bw2 = body_w * 0.5f, bh2 = body_h * 0.5f;

    /* Body */
    draw_filled_rect(d, cx - bw2, cy - bh2, cx + bw2, cy + bh2,
                     0.14f, 0.14f, 0.17f, 1.0f);
    /* Roof */
    float rw2 = body_w * 0.55f * 0.5f, rh2 = body_h * 0.40f * 0.5f;
    draw_filled_rect(d, cx - rw2, cy - rh2, cx + rw2, cy + rh2,
                     0.26f, 0.26f, 0.30f, 1.0f);
    /* Windshield */
    draw_filled_rect(d, cx - rw2 * 0.7f, cy + rh2,
                        cx + rw2 * 0.7f, cy + rh2 + 0.015f,
                        0.20f, 0.45f, 0.65f, 1.0f);
}

/* Left sidebar: dark panel + text placeholder bar + button outlines + dots */
static void draw_sidebar(display_t *d, float x0, float x1)
{
    float y_top = 0.95f, y_bot = -0.95f, m = 0.015f;

    /* Background */
    draw_filled_rect(d, x0, y_bot, x1, y_top, 0.04f, 0.04f, 0.05f, 1.0f);

    /* Separator line */
    draw_filled_rect(d, x1 - 0.002f, y_bot, x1 + 0.002f, y_top,
                     0.16f, 0.16f, 0.18f, 1.0f);

    /* "请注意安全" text placeholder (white bar) */
    draw_filled_rect(d, x0 + m, 0.85f, x1 - m, 0.92f,
                     0.85f, 0.85f, 0.85f, 1.0f);

    /* 4 button placeholders with green indicator dots */
    float bh = 0.055f, gap = 0.018f, by = 0.65f;
    for (int i = 0; i < 4; i++) {
        float b0 = by - bh, b1 = by;
        draw_filled_rect(d, x0 + m, b0, x1 - m, b1, 0.10f, 0.10f, 0.11f, 1.0f);
        draw_outline_rect(d, x0 + m, b0, x1 - m, b1, 0.22f, 0.22f, 0.24f, 1.0f);
        draw_filled_rect(d, x0 + m + 0.004f, b0 + 0.008f,
                            x0 + m + 0.014f, b1 - 0.008f,
                            0.18f, 0.55f, 0.18f, 1.0f);
        by = b0 - gap;
    }

    /* Bottom indicator dots */
    float dy = -0.65f;
    for (int i = 0; i < 4; i++) {
        draw_filled_rect(d, x0 + 0.025f, dy, x0 + 0.045f, dy + 0.025f,
                         0.25f, 0.25f, 0.55f, 1.0f);
        dy -= 0.045f;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void disp_set_detections(display_t *d, const detection_t *dets, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->det_lock);
    /* Only clear channels that have new data in this batch */
    bool has_new[4] = {false, false, false, false};
    for (int i = 0; i < n; i++) {
        int cam = dets[i].cam_idx;
        if (cam >= 0 && cam < 4) has_new[cam] = true;
    }
    for (int c = 0; c < 4; c++) {
        if (has_new[c]) d->det_count[c] = 0;
    }
    for (int i = 0; i < n; i++) {
        int cam = dets[i].cam_idx;
        if (cam < 0 || cam >= 4) continue;
        if (d->det_count[cam] >= MAX_DETECTIONS) continue;
        d->dets[cam][d->det_count[cam]++] = dets[i];
    }
    pthread_mutex_unlock(&d->det_lock);
}

display_t *disp_open(int width, int height, int n_cameras)
{
    display_t *d = (display_t *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;

    d->w = width;
    d->h = height;
    d->n_cams = (n_cameras > 4) ? 4 : n_cameras;

    /* ---- Wayland connect ---- */
    const char *wl_sock = getenv("WAYLAND_DISPLAY");
    if (!wl_sock)
        wl_sock = "/var/run/wayland-0";
    d->display = wl_display_connect(wl_sock);
    if (!d->display) {
        d->display = wl_display_connect(NULL);   /* fallback to default */
    }
    if (!d->display) {
        perror("[display] wl_display_connect");
        goto fail;
    }

    struct wl_registry *reg = wl_display_get_registry(d->display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(d->display);

    if (!g_compositor) {
        fprintf(stderr, "[display] no wl_compositor found\n");
        goto fail;
    }

    d->surface = wl_compositor_create_surface(g_compositor);
    if (!d->surface) {
        fprintf(stderr, "[display] wl_compositor_create_surface failed\n");
        goto fail;
    }

    /* ---- xdg-shell setup ---- */
    if (g_wm_base) {
        d->xdg_surface = xdg_wm_base_get_xdg_surface(g_wm_base, d->surface);
        xdg_surface_add_listener(d->xdg_surface,
                                 &xdg_surface_listener, d);

        d->xdg_toplevel = xdg_surface_get_toplevel(d->xdg_surface);
        xdg_toplevel_add_listener(d->xdg_toplevel,
                                  &xdg_toplevel_listener, d);
        xdg_toplevel_set_title(d->xdg_toplevel, "Camera");
        xdg_toplevel_set_maximized(d->xdg_toplevel);
        wl_surface_commit(d->surface);

        /* Wait for configure ack (up to 2 s) */
        int retries = 0;
        while (!d->configured && retries < 200) {
            wl_display_dispatch(d->display);
            retries++;
        }
        printf("[display] xdg-shell configured=%d\n", d->configured);
    } else {
        printf("[display] no xdg_wm_base, using raw surface\n");
    }

    /* ---- wl_egl_window ---- */
    d->egl_window = wl_egl_window_create(d->surface, width, height);
    if (!d->egl_window) {
        fprintf(stderr, "[display] wl_egl_window_create failed\n");
        goto fail;
    }

    /* ---- EGL initialisation ---- */
    d->egl_dpy = eglGetDisplay((EGLNativeDisplayType)d->display);
    if (d->egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "[display] eglGetDisplay failed\n");
        goto fail;
    }
    eglInitialize(d->egl_dpy, NULL, NULL);

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint    n;
    if (!eglChooseConfig(d->egl_dpy, cfg_attr, &cfg, 1, &n)) {
        fprintf(stderr, "[display] eglChooseConfig failed\n");
        goto fail;
    }

    EGLint ctx_attr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    d->egl_ctx = eglCreateContext(d->egl_dpy, cfg,
                                  EGL_NO_CONTEXT, ctx_attr);
    if (!d->egl_ctx) {
        fprintf(stderr, "[display] eglCreateContext failed\n");
        goto fail;
    }

    d->egl_surf = eglCreateWindowSurface(d->egl_dpy, cfg,
                                          (EGLNativeWindowType)d->egl_window,
                                          NULL);
    if (!d->egl_surf) {
        fprintf(stderr, "[display] eglCreateWindowSurface failed\n");
        goto fail;
    }

    eglMakeCurrent(d->egl_dpy, d->egl_surf, d->egl_surf, d->egl_ctx);

    /* ---- Compile YUV shader ---- */
    d->program = glCreateProgram();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    glAttachShader(d->program, vs);
    glAttachShader(d->program, fs);
    glLinkProgram(d->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    /* ---- Display mode selection (AVM_MODE > FISHEYE_MODE > grid) ---- */
    {
        const char *avm = getenv("AVM_MODE");
        const char *fm  = getenv("FISHEYE_MODE");
        if (avm && avm[0] == '1')
            d->mode = DISPLAY_MODE_AVM;
        else if (fm && fm[0] == '1')
            d->mode = DISPLAY_MODE_FISHEYE_GRID;
        else
            d->mode = DISPLAY_MODE_GRID;
    }

    /* ---- Compile mesh shader (shared by FISHEYE_GRID and AVM) ---- */
    if (d->mode == DISPLAY_MODE_FISHEYE_GRID || d->mode == DISPLAY_MODE_AVM) {
        d->mesh_prog = glCreateProgram();
        { GLuint v = compile_shader(GL_VERTEX_SHADER, vert_src);
          GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag_src);
          glAttachShader(d->mesh_prog, v);
          glAttachShader(d->mesh_prog, f);
          glLinkProgram(d->mesh_prog);
          glDeleteShader(v); glDeleteShader(f); }
        /* Cache locations once — no per-frame glGet*Location calls */
        d->mesh_uloc_y   = glGetUniformLocation(d->mesh_prog, "u_texY");
        d->mesh_uloc_uv  = glGetUniformLocation(d->mesh_prog, "u_texUV");
        d->mesh_aloc_pos = glGetAttribLocation(d->mesh_prog, "a_pos");
        d->mesh_aloc_tex = glGetAttribLocation(d->mesh_prog, "a_tex");
    }

    /* ---- Per-camera params: FOV, rotate, flip (env overrides) ---- */
    float fov_cam[4]    = { 124.0f, 155.0f, 161.0f, 170.0f };
    int   rot_cam[4]    = { 0, 0, 0, 0 };
    bool  flipx_cam[4]  = { false, false, false, false };
    bool  flipy_cam[4]  = { false, false, false, false };

    const char *ff = getenv("FISHEYE_FOV");
    if (ff) {
        char buf[64]; strncpy(buf, ff, 63); buf[63] = 0;
        char *tok = strtok(buf, ",");
        for (int j = 0; j < 4 && tok; j++, tok = strtok(NULL, ","))
            fov_cam[j] = atof(tok);
    }
    const char *fr = getenv("FISHEYE_ROTATE");
    if (fr) {
        char buf[64]; strncpy(buf, fr, 63); buf[63] = 0;
        char *tok = strtok(buf, ",");
        for (int j = 0; j < 4 && tok; j++, tok = strtok(NULL, ","))
            rot_cam[j] = atoi(tok);
    }
    const char *ffx = getenv("FISHEYE_FLIPX");
    if (ffx) {
        char buf[64]; strncpy(buf, ffx, 63); buf[63] = 0;
        char *tok = strtok(buf, ",");
        for (int j = 0; j < 4 && tok; j++, tok = strtok(NULL, ","))
            flipx_cam[j] = (atoi(tok) != 0);
    }
    const char *ffy = getenv("FISHEYE_FLIPY");
    if (ffy) {
        char buf[64]; strncpy(buf, ffy, 63); buf[63] = 0;
        char *tok = strtok(buf, ",");
        for (int j = 0; j < 4 && tok; j++, tok = strtok(NULL, ","))
            flipy_cam[j] = (atoi(tok) != 0);
    }

    /* Debug: single camera fullscreen */
    const char *fdc = getenv("FISHEYE_DEBUG_CAM");
    int debug_cam = fdc ? atoi(fdc) : -1;
    if (debug_cam >= 0 && debug_cam < d->n_cams) {
        printf("[display] DEBUG: single camera %d fullscreen\n", debug_cam);
    }

    /* ---- Build meshes ---- */
    fisheye_uv_stats_t uv_stats[4];

    if (d->mode == DISPLAY_MODE_FISHEYE_GRID) {
        int cols = (d->n_cams <= 2) ? d->n_cams : 2;
        int rows = (d->n_cams <= 2) ? 1 : 2;
        float qw = 2.0f / cols, qh = 2.0f / rows;

        printf("[display] fisheye grid params:\n");
        for (int i = 0; i < d->n_cams; i++) {
            int col = (debug_cam >= 0) ? 0 : i % cols;
            int row = (debug_cam >= 0) ? 0 : i / cols;
            float x0 = -1.0f + col * ((debug_cam >= 0) ? 2.0f : qw);
            float y0 =  1.0f - (row + 1) * ((debug_cam >= 0) ? 2.0f : qh);
            float tw = (debug_cam >= 0) ? 2.0f : qw;
            float th = (debug_cam >= 0) ? 2.0f : qh;

            printf("  cam%d: fov=%.0f rot=%d flip=%d,%d rect=[%.2f,%.2f,%.2f,%.2f]\n",
                   i, fov_cam[i], rot_cam[i], flipx_cam[i], flipy_cam[i],
                   x0, y0, tw, th);

            fisheye_mesh_t m;
            if (fisheye_mesh_build_ex(&m, &g_fisheye_cams[i],
                                      x0, y0, tw, th,
                                      (debug_cam >= 0) ? 1920 : 960,
                                      (debug_cam >= 0) ? 1080 : 540,
                                      fov_cam[i],
                                      rot_cam[i], flipx_cam[i], flipy_cam[i],
                                      &uv_stats[i]) == 0) {
                /* Dump UV debug PPM (before debug_cam skip, so all cams get PPM) */
                { char path[64];
                  snprintf(path, sizeof(path), "/tmp/fisheye_cam%d_uv.ppm", i);
                  fisheye_mesh_dump_uv_debug(path, &uv_stats[i], &g_fisheye_cams[i],
                                             fov_cam[i], 640, 360,
                                             rot_cam[i], flipx_cam[i], flipy_cam[i]); }

                if (debug_cam >= 0 && i != debug_cam) continue;
                d->grid_vbo_pos[i] = m.vbo_pos;
                d->grid_vbo_tex[i] = m.vbo_tex;
                d->grid_ibo[i]     = m.ibo;
                d->grid_nidx[i]    = m.num_indices;
            }
        }
        printf("[display] fisheye grid mode ON (%d cameras)\n", d->n_cams);
    }

    if (d->mode == DISPLAY_MODE_AVM) {
        const float sb_w  = 0.24f;
        const float mx0   = -1.0f + sb_w;
        const float mx1   =  1.0f;
        const float mh    = 2.0f;
        const float mcx   = (mx0 + mx1) * 0.5f;
        const float mcy   = 0.0f;
        const float veh_w = 0.10f;
        const float veh_h = 0.18f;

        float tiles[4][4] = {
            { mcx - veh_w,  mcy + veh_h,       mcx + veh_w,  mcy + veh_h + mh * 0.38f },
            { mcx + veh_w,  mcy - veh_h * 0.5f, mx1,          mcy + veh_h * 0.5f },
            { mcx - veh_w,  mcy - veh_h - mh * 0.38f, mcx + veh_w,  mcy - veh_h },
            { mx0,          mcy - veh_h * 0.5f, mcx - veh_w,  mcy + veh_h * 0.5f },
        };

        printf("[display] AVM params:\n");
        for (int i = 0; i < d->n_cams; i++) {
            float tx0, ty0, tw, th;
            if (debug_cam >= 0) {
                tx0 = -1.0f; ty0 = -1.0f; tw = 2.0f; th = 2.0f;
            } else {
                tx0 = tiles[i][0]; ty0 = tiles[i][1];
                tw  = tiles[i][2] - tx0;
                th  = tiles[i][3] - ty0;
            }

            printf("  cam%d: fov=%.0f rot=%d flip=%d,%d rect=[%.2f,%.2f,%.2f,%.2f]\n",
                   i, fov_cam[i], rot_cam[i], flipx_cam[i], flipy_cam[i],
                   tx0, ty0, tw, th);

            fisheye_mesh_t m;
            if (fisheye_mesh_build_ex(&m, &g_fisheye_cams[i],
                                      tx0, ty0, tw, th,
                                      (debug_cam >= 0) ? 1920 : 480,
                                      (debug_cam >= 0) ? 1080 : 360,
                                      fov_cam[i],
                                      rot_cam[i], flipx_cam[i], flipy_cam[i],
                                      &uv_stats[i]) == 0) {
                { char path[64];
                  snprintf(path, sizeof(path), "/tmp/fisheye_cam%d_uv.ppm", i);
                  fisheye_mesh_dump_uv_debug(path, &uv_stats[i], &g_fisheye_cams[i],
                                             fov_cam[i], 640, 360,
                                             rot_cam[i], flipx_cam[i], flipy_cam[i]); }

                if (debug_cam >= 0 && i != debug_cam) continue;
                d->avm_vbo_pos[i] = m.vbo_pos;
                d->avm_vbo_tex[i] = m.vbo_tex;
                d->avm_ibo[i]     = m.ibo;
                d->avm_nidx[i]    = m.num_indices;
            }
        }
        printf("[display] AVM mode ON (%d cameras)\n", d->n_cams);
    }

    /* OSD program (flat color for detection boxes) */
    d->osd_prog = glCreateProgram();
    { GLuint v = compile_shader(GL_VERTEX_SHADER, osd_vert);
      GLuint f = compile_shader(GL_FRAGMENT_SHADER, osd_frag);
      glAttachShader(d->osd_prog, v); glAttachShader(d->osd_prog, f);
      glLinkProgram(d->osd_prog); glDeleteShader(v); glDeleteShader(f); }
    d->osd_pos = glGetAttribLocation(d->osd_prog, "a_pos");
    d->osd_color_loc = glGetUniformLocation(d->osd_prog, "u_color");

    d->loc_pos = (GLuint)glGetAttribLocation(d->program, "a_pos");
    d->loc_tex = (GLuint)glGetAttribLocation(d->program, "a_tex");

    /* ---- Pre-allocate GL textures (one pair per camera) ---- */
    for (int i = 0; i < d->n_cams; i++) {
        glGenTextures(1, &d->texY[i]);
        glGenTextures(1, &d->texUV[i]);
    }

    pthread_mutex_init(&d->det_lock, NULL);

    /* Black clear (no green flash at start) */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(d->egl_dpy, d->egl_surf);
    wl_display_flush(d->display);

    printf("[display] Wayland+EGL init %dx%d, %d cameras\n",
           width, height, d->n_cams);
    return d;

fail:
    /* Partial cleanup on error */
    if (d->egl_surf)
        eglDestroySurface(d->egl_dpy, d->egl_surf);
    if (d->egl_ctx)
        eglDestroyContext(d->egl_dpy, d->egl_ctx);
    if (d->egl_dpy != EGL_NO_DISPLAY)
        eglTerminate(d->egl_dpy);
    if (d->egl_window)
        wl_egl_window_destroy(d->egl_window);
    if (d->xdg_toplevel)
        xdg_toplevel_destroy(d->xdg_toplevel);
    if (d->xdg_surface)
        xdg_surface_destroy(d->xdg_surface);
    if (d->surface)
        wl_surface_destroy(d->surface);
    if (d->display)
        wl_display_disconnect(d->display);
    free(d);
    return NULL;
}

/*
 * Update one camera's texture pair from a frame_t.
 *
 * The caller guarantees that f->fd is valid for the duration of this
 * call.  The fd is NOT closed or consumed here; ownership remains with
 * the capture pipeline.
 */
void disp_update(display_t *d, int cam_idx, const frame_t *f)
{
    if (!d || cam_idx < 0 || cam_idx >= d->n_cams || !f)
        return;
    import_nv12(d, cam_idx, f);
}

/*
 * Render one camera tile using the mesh program + VBOs.
 * Uses cached uniform/attribute locations for zero per-frame lookup cost.
 * d->mesh_prog must already be active (glUseProgram called by caller).
 */
static void draw_mesh_tile(display_t *d, int i,
                            GLuint vbo_pos, GLuint vbo_tex,
                            GLuint ibo, int nidx)
{
    if (!d->has_frame[i]) return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->texY[i]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
    glUniform1i(d->mesh_uloc_y, 0);
    glUniform1i(d->mesh_uloc_uv, 1);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glVertexAttribPointer(d->mesh_aloc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(d->mesh_aloc_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_tex);
    glVertexAttribPointer(d->mesh_aloc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(d->mesh_aloc_tex);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, 0);
    glDisableVertexAttribArray(d->mesh_aloc_pos);
    glDisableVertexAttribArray(d->mesh_aloc_tex);
}

/* ---- Mode-specific draw functions ---- */

static void draw_grid_mode(display_t *d)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(d->program);
    glViewport(0, 0, d->w, d->h);

    int cols = (d->n_cams <= 2) ? d->n_cams : 2;
    int rows = (d->n_cams <= 2) ? 1 : 2;
    float qw = 2.0f / cols, qh = 2.0f / rows;

    GLint uy  = glGetUniformLocation(d->program, "u_texY");
    GLint uuv = glGetUniformLocation(d->program, "u_texUV");

    for (int i = 0; i < d->n_cams; i++) {
        if (!d->has_frame[i]) continue;
        int col = i % cols, row = i / cols;
        float x0 = -1.0f + col * qw, x1 = x0 + qw;
        float y1 =  1.0f - row * qh, y0 = y1 - qh;

        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d->texY[i]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
        glUniform1i(uy, 0); glUniform1i(uuv, 1);

        float v[] = { x0,y0,0,0, x1,y0,1,0, x1,y1,1,1, x0,y1,0,1 };
        glVertexAttribPointer(d->loc_pos, 2, GL_FLOAT, GL_FALSE, 16, v);
        glVertexAttribPointer(d->loc_tex, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
        glEnableVertexAttribArray(d->loc_pos);
        glEnableVertexAttribArray(d->loc_tex);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(d->loc_pos);
        glDisableVertexAttribArray(d->loc_tex);
    }
    /* Detection overlay handled by caller */
}

static void draw_fisheye_grid_mode(display_t *d)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(d->mesh_prog);
    glViewport(0, 0, d->w, d->h);

    for (int i = 0; i < d->n_cams; i++) {
        draw_mesh_tile(d, i,
                       d->grid_vbo_pos[i], d->grid_vbo_tex[i],
                       d->grid_ibo[i], d->grid_nidx[i]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void draw_avm_mode(display_t *d)
{
    /* Black background */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, d->w, d->h);

    /* Sidebar */
    const float sb_x0 = -1.0f, sb_x1 = -1.0f + 0.24f;
    glUseProgram(d->osd_prog);
    draw_sidebar(d, sb_x0, sb_x1);

    /* 4 fisheye-corrected camera views */
    glUseProgram(d->mesh_prog);
    for (int i = 0; i < d->n_cams; i++) {
        draw_mesh_tile(d, i,
                       d->avm_vbo_pos[i], d->avm_vbo_tex[i],
                       d->avm_ibo[i], d->avm_nidx[i]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* Vehicle placeholder at center of main area */
    const float mcx = (sb_x1 + 1.0f) * 0.5f, mcy = 0.0f;
    glUseProgram(d->osd_prog);
    draw_vehicle_placeholder(d, mcx, mcy, 0.12f, 0.22f);
}

/*
 * Render the current frame to the window.
 */
void disp_draw(display_t *d)
{
    if (!d) return;

    switch (d->mode) {
    case DISPLAY_MODE_GRID:          draw_grid_mode(d);         break;
    case DISPLAY_MODE_FISHEYE_GRID:  draw_fisheye_grid_mode(d); break;
    case DISPLAY_MODE_AVM:           draw_avm_mode(d);          break;
    }

    /* Detection overlay (grid modes only — AVM can add later) */
    if (d->mode != DISPLAY_MODE_AVM) {
        int cols = (d->n_cams <= 2) ? d->n_cams : 2;
        int rows = (d->n_cams <= 2) ? 1 : 2;
        float qw = 2.0f / cols, qh = 2.0f / rows;

        pthread_mutex_lock(&d->det_lock);
        glUseProgram(d->osd_prog);
        glLineWidth(3.0f);

        static const float ch_colors[4][4] = {
            {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1},
        };
        for (int cam = 0; cam < d->n_cams && cam < 4; cam++) {
            if (!d->has_frame[cam] || d->det_count[cam] <= 0) continue;

            int col = cam % cols, row = cam / cols;
            float tx0 = -1.0f + col * qw;
            float ty1 =  1.0f - row * qh;
            float xs = qw / 1920.0f, ys = qh / 1080.0f;

            glUniform4fv(d->osd_color_loc, 1, ch_colors[cam]);
            for (int i = 0; i < d->det_count[cam]; i++) {
                detection_t *dt = &d->dets[cam][i];
                if (dt->class_id != 0) continue;
                float bx = tx0 + dt->x * xs;
                float by = ty1 - (dt->y + dt->h) * ys;
                float bw = dt->w * xs, bh = dt->h * ys;
                float v[] = { bx,by, bx+bw,by, bx+bw,by+bh, bx,by+bh };
                glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
                glEnableVertexAttribArray(d->osd_pos);
                glDrawArrays(GL_LINE_LOOP, 0, 4);
                glDisableVertexAttribArray(d->osd_pos);
            }
        }
        pthread_mutex_unlock(&d->det_lock);
    }

    /* ---- Live frame grab (FISHEYE_LIVE_DUMP=1) ---- */
    {
        static int live_frame_seq = 0;
        static int live_dump_checked = 0;
        static int live_dump_enabled = 0;
        if (!live_dump_checked) {
            const char *ld = getenv("FISHEYE_LIVE_DUMP");
            live_dump_enabled = (ld && ld[0] == '1');
            live_dump_checked = 1;
        }
        if (live_dump_enabled && live_frame_seq < 1) {
            int w = d->w, h = d->h;
            unsigned char *px = (unsigned char *)malloc(w * h * 3);
            if (px) {
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
                /* glReadPixels gives bottom-up; flip for PPM */
                char path[64];
                snprintf(path, sizeof(path), "/tmp/fisheye_live_%04d.ppm", live_frame_seq);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    fprintf(fp, "P6\n%d %d\n255\n", w, h);
                    for (int y = h - 1; y >= 0; y--)
                        fwrite(px + y * w * 3, 1, w * 3, fp);
                    fclose(fp);
                    printf("[display] live frame saved: %s (%dx%d)\n", path, w, h);
                }
                free(px);
                live_frame_seq++;
            }
        }
    }

    eglSwapBuffers(d->egl_dpy, d->egl_surf);
}

/*
 * Dispatch pending Wayland events (non-blocking).
 * Call this periodically from the main loop.
 */
void disp_dispatch(display_t *d)
{
    if (!d || !d->display)
        return;

    wl_display_flush(d->display);
    wl_display_dispatch_pending(d->display);

    /* Non-blocking poll to keep the connection alive */
    struct pollfd pfd = {
        .fd     = wl_display_get_fd(d->display),
        .events = POLLIN,
    };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        wl_display_dispatch(d->display);
}

/*
 * Tear down all display resources.
 */
void disp_close(display_t *d)
{
    if (!d)
        return;

    /* Ensure EGL context is current before cleanup */
    eglMakeCurrent(d->egl_dpy, d->egl_surf, d->egl_surf, d->egl_ctx);

    for (int i = 0; i < d->n_cams; i++) {
        glDeleteTextures(1, &d->texY[i]);
        glDeleteTextures(1, &d->texUV[i]);
    }
    glDeleteProgram(d->program);
    glDeleteProgram(d->osd_prog);
    if (d->mode == DISPLAY_MODE_FISHEYE_GRID || d->mode == DISPLAY_MODE_AVM) {
        glDeleteProgram(d->mesh_prog);
    }
    for (int i = 0; i < d->n_cams; i++) {
        if (d->grid_vbo_pos[i]) glDeleteBuffers(1, &d->grid_vbo_pos[i]);
        if (d->grid_vbo_tex[i]) glDeleteBuffers(1, &d->grid_vbo_tex[i]);
        if (d->grid_ibo[i])     glDeleteBuffers(1, &d->grid_ibo[i]);
        if (d->avm_vbo_pos[i])  glDeleteBuffers(1, &d->avm_vbo_pos[i]);
        if (d->avm_vbo_tex[i])  glDeleteBuffers(1, &d->avm_vbo_tex[i]);
        if (d->avm_ibo[i])      glDeleteBuffers(1, &d->avm_ibo[i]);
    }

    if (d->egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(d->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (d->egl_surf)
            eglDestroySurface(d->egl_dpy, d->egl_surf);
        if (d->egl_ctx)
            eglDestroyContext(d->egl_dpy, d->egl_ctx);
        eglTerminate(d->egl_dpy);
    }

    if (d->egl_window)
        wl_egl_window_destroy(d->egl_window);
    if (d->xdg_toplevel)
        xdg_toplevel_destroy(d->xdg_toplevel);
    if (d->xdg_surface)
        xdg_surface_destroy(d->xdg_surface);
    if (d->surface)
        wl_surface_destroy(d->surface);
    if (d->display)
        wl_display_disconnect(d->display);

    pthread_mutex_destroy(&d->det_lock);
    free(d);
    printf("[display] closed\n");
}
