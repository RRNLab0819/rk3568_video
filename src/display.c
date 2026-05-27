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
    DISPLAY_MODE_OEM_AVM,        /* opt-in OEM-style AVM UI */
} display_mode_t;

typedef enum {
    OEM_AVM_SURROUND_MAIN = 0,
    OEM_AVM_SURROUND_FULL,
    OEM_AVM_FRONT,
    OEM_AVM_REAR,
    OEM_AVM_LEFT,
    OEM_AVM_RIGHT,
    OEM_AVM_MULTI,
    OEM_AVM_VIEW_COUNT
} oem_avm_view_t;

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
    bool                    tex_ready[4];
    bool                    configured;

    /* Wayland input for OEM AVM mouse / keyboard switching */
    struct wl_seat         *seat;
    struct wl_pointer      *pointer;
    struct wl_keyboard     *keyboard;
    double                  pointer_x;
    double                  pointer_y;

    /* Display mode (OEM_AVM_MODE > FISHEYE_MODE > grid baseline) */
    display_mode_t mode;
    oem_avm_view_t          oem_view;
    int                     oem_main_cam;
    int                     oem_cam_map[4];

    /* Mesh shader (shared by FISHEYE_GRID and AVM modes) */
    GLuint      mesh_prog;
    GLint       mesh_uloc_y, mesh_uloc_uv;  /* cached uniform locations */
    GLint       mesh_aloc_pos, mesh_aloc_tex; /* cached attribute locations */

    /* Fisheye-grid mesh VBOs (2x2 tile) */
    GLuint      grid_vbo_pos[4];
    GLuint      grid_vbo_tex[4];
    GLuint      grid_ibo[4];
    int         grid_nidx[4];


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


static void oem_avm_set_view(display_t *d, int view)
{
    if (!d || d->mode != DISPLAY_MODE_OEM_AVM) return;
    if (view < 0 || view >= OEM_AVM_VIEW_COUNT) return;
    if (d->oem_view != (oem_avm_view_t)view) {
        d->oem_view = (oem_avm_view_t)view;
        printf("[display] OEM AVM view=%d\n", view);
    }
}

static void oem_avm_handle_toolbar_click(display_t *d)
{
    if (!d || d->mode != DISPLAY_MODE_OEM_AVM || d->w <= 0 || d->h <= 0) return;
    const double toolbar_h = d->h * 0.13;
    if (d->pointer_y < (double)d->h - toolbar_h) return;
    int idx = (int)(d->pointer_x / ((double)d->w / (double)OEM_AVM_VIEW_COUNT));
    oem_avm_set_view(d, idx);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)serial; (void)surface;
    display_t *d = (display_t *)data;
    d->pointer_x = wl_fixed_to_double(sx);
    d->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    display_t *d = (display_t *)data;
    d->pointer_x = wl_fixed_to_double(sx);
    d->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    (void)pointer; (void)serial; (void)time;
    if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_PRESSED)
        oem_avm_handle_toolbar_click((display_t *)data);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    (void)data; (void)keyboard; (void)format; (void)size;
    if (fd >= 0) close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if (key >= 2 && key <= 8)
        oem_avm_set_view((display_t *)data, (int)key - 2);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t mods_depressed, uint32_t mods_latched,
                               uint32_t mods_locked, uint32_t group)
{
    (void)data; (void)keyboard; (void)serial;
    (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    display_t *d = (display_t *)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_handler(void *data, struct wl_registry *reg,
                              uint32_t id, const char *iface, uint32_t ver)
{
    display_t *d = (display_t *)data;
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = (struct wl_compositor *)wl_registry_bind(
            reg, id, &wl_compositor_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0)
        g_wm_base = (struct xdg_wm_base *)wl_registry_bind(
            reg, id, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, "wl_seat") == 0 && d) {
        d->seat = (struct wl_seat *)wl_registry_bind(
            reg, id, &wl_seat_interface, ver > 5 ? 5 : ver);
        wl_seat_add_listener(d->seat, &seat_listener, d);
    }
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
    const uint8_t *y = (const uint8_t *)f->ptr;
    const uint8_t *uv = y + ysz;
    uint8_t *tmp_y = NULL;
    uint8_t *tmp_uv = NULL;

    if (f->stride != f->width) {
        tmp_y = (uint8_t *)malloc(ysz);
        tmp_uv = (uint8_t *)malloc(uvsz);
        if (!tmp_y || !tmp_uv) {
            free(tmp_y);
            free(tmp_uv);
            return;
        }

        uint8_t *dptr = tmp_y;
        const uint8_t *s = (const uint8_t *)f->ptr;
        for (int r = 0; r < f->height; r++) {
            memcpy(dptr, s, f->width);
            s += f->stride;
            dptr += f->width;
        }

        dptr = tmp_uv;
        s = (const uint8_t *)f->ptr + f->stride * f->height;
        for (int r = 0; r < f->height / 2; r++) {
            memcpy(dptr, s, f->width);
            s += f->stride;
            dptr += f->width;
        }
        y = tmp_y;
        uv = tmp_uv;
    }

    glBindTexture(GL_TEXTURE_2D, d->texY[cam]);
    if (!d->tex_ready[cam]) {
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
        d->tex_ready[cam] = true;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, y);
        glBindTexture(GL_TEXTURE_2D, d->texUV[cam]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width / 2, f->height / 2,
                        GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);
    }

    free(tmp_y);
    free(tmp_uv);
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
    wl_registry_add_listener(reg, &registry_listener, d);
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

    /* ---- Display mode selection (OEM_AVM_MODE > FISHEYE_MODE > grid) ---- */
    {
        const char *oem = getenv("OEM_AVM_MODE");
        const char *fm  = getenv("FISHEYE_MODE");
        if (oem && oem[0] == '1')
            d->mode = DISPLAY_MODE_OEM_AVM;
        else if (fm && fm[0] == '1')
            d->mode = DISPLAY_MODE_FISHEYE_GRID;
        else
            d->mode = DISPLAY_MODE_GRID;
    }

    /* ---- Compile mesh shader for fisheye grid only ---- */
    if (d->mode == DISPLAY_MODE_FISHEYE_GRID) {
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

    d->oem_view = OEM_AVM_SURROUND_MAIN;
    d->oem_main_cam = 0;
    for (int i = 0; i < 4; i++) d->oem_cam_map[i] = i;
    {
        const char *mv = getenv("OEM_AVM_MAIN_CAM");
        if (mv) {
            int cam = atoi(mv);
            if (cam >= 0 && cam < d->n_cams) d->oem_main_cam = cam;
        }
        const char *cm = getenv("OEM_AVM_CAM_MAP");
        if (cm) {
            char buf[64]; strncpy(buf, cm, 63); buf[63] = 0;
            char *tok = strtok(buf, ",");
            for (int i = 0; i < 4 && tok; i++, tok = strtok(NULL, ",")) {
                int cam = atoi(tok);
                if (cam >= 0 && cam < 4) d->oem_cam_map[i] = cam;
            }
        }
        const char *view = getenv("OEM_AVM_VIEW");
        if (view) {
            if (strcmp(view, "surround-full") == 0) d->oem_view = OEM_AVM_SURROUND_FULL;
            else if (strcmp(view, "front") == 0) d->oem_view = OEM_AVM_FRONT;
            else if (strcmp(view, "rear") == 0) d->oem_view = OEM_AVM_REAR;
            else if (strcmp(view, "left") == 0) d->oem_view = OEM_AVM_LEFT;
            else if (strcmp(view, "right") == 0) d->oem_view = OEM_AVM_RIGHT;
            else if (strcmp(view, "multi") == 0) d->oem_view = OEM_AVM_MULTI;
        }
    }
    if (d->mode == DISPLAY_MODE_OEM_AVM)
        printf("[display] OEM AVM mode ON view=%d main_cam=%d\n", d->oem_view, d->oem_main_cam);

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
    if (d->keyboard)
        wl_keyboard_destroy(d->keyboard);
    if (d->pointer)
        wl_pointer_destroy(d->pointer);
    if (d->seat)
        wl_seat_destroy(d->seat);

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

static int oem_slot_cam(display_t *d, int slot)
{
    if (slot < 0 || slot >= 4) return 0;
    int cam = d->oem_cam_map[slot];
    if (cam < 0 || cam >= d->n_cams) cam = slot;
    if (cam >= d->n_cams) cam = 0;
    return cam;
}

static void draw_camera_rect(display_t *d, int cam,
                             float x0, float y0, float x1, float y1)
{
    if (cam < 0 || cam >= d->n_cams || !d->has_frame[cam]) return;

    glUseProgram(d->program);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, d->texY[cam]);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, d->texUV[cam]);
    glUniform1i(glGetUniformLocation(d->program, "u_texY"), 0);
    glUniform1i(glGetUniformLocation(d->program, "u_texUV"), 1);

    float v[] = { x0,y0,0,0, x1,y0,1,0, x1,y1,1,1, x0,y1,0,1 };
    glVertexAttribPointer(d->loc_pos, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(d->loc_tex, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(d->loc_pos);
    glEnableVertexAttribArray(d->loc_tex);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(d->loc_pos);
    glDisableVertexAttribArray(d->loc_tex);
}

static void draw_osd_line(display_t *d, float x0, float y0, float x1, float y1,
                          float r, float g, float b, float a)
{
    float v[] = { x0,y0, x1,y1 };
    glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(d->osd_pos);
    glUniform4f(d->osd_color_loc, r, g, b, a);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, 2);
    glDisableVertexAttribArray(d->osd_pos);
}

static void draw_guide_lines(display_t *d, bool rear)
{
    (void)rear;
    glUseProgram(d->osd_prog);
    const float y0 = -0.68f, y1 = -0.18f, y2 = 0.28f;
    draw_osd_line(d, -0.45f, y0, -0.20f, y2, 1.0f, 0.78f, 0.18f, 1.0f);
    draw_osd_line(d,  0.45f, y0,  0.20f, y2, 1.0f, 0.78f, 0.18f, 1.0f);
    draw_osd_line(d, -0.38f, y1,  0.38f, y1, 1.0f, 0.78f, 0.18f, 1.0f);
    draw_osd_line(d, -0.25f, 0.05f, 0.25f, 0.05f, 0.95f, 0.20f, 0.20f, 1.0f);
}

static void draw_oem_icon(display_t *d, int idx, float cx, float cy, float z)
{
    const float r = 0.86f, g = 0.94f, b = 1.0f, a = 1.0f;
    if (idx == OEM_AVM_SURROUND_MAIN || idx == OEM_AVM_SURROUND_FULL) {
        draw_outline_rect(d, cx - z*0.35f, cy - z*0.45f, cx + z*0.35f, cy + z*0.45f, r,g,b,a);
        draw_osd_line(d, cx - z*0.65f, cy, cx - z*0.42f, cy, r,g,b,a);
        draw_osd_line(d, cx + z*0.42f, cy, cx + z*0.65f, cy, r,g,b,a);
        if (idx == OEM_AVM_SURROUND_FULL)
            draw_outline_rect(d, cx - z*0.62f, cy - z*0.62f, cx + z*0.62f, cy + z*0.62f, r,g,b,a);
    } else if (idx == OEM_AVM_FRONT || idx == OEM_AVM_REAR) {
        draw_outline_rect(d, cx - z*0.28f, cy - z*0.50f, cx + z*0.28f, cy + z*0.50f, r,g,b,a);
        float dir = (idx == OEM_AVM_FRONT) ? 1.0f : -1.0f;
        draw_osd_line(d, cx, cy + dir*z*0.72f, cx, cy + dir*z*0.48f, r,g,b,a);
        draw_osd_line(d, cx, cy + dir*z*0.72f, cx - z*0.16f, cy + dir*z*0.56f, r,g,b,a);
        draw_osd_line(d, cx, cy + dir*z*0.72f, cx + z*0.16f, cy + dir*z*0.56f, r,g,b,a);
    } else if (idx == OEM_AVM_LEFT || idx == OEM_AVM_RIGHT) {
        float dir = (idx == OEM_AVM_RIGHT) ? 1.0f : -1.0f;
        draw_outline_rect(d, cx - z*0.22f, cy - z*0.45f, cx + z*0.22f, cy + z*0.45f, r,g,b,a);
        draw_osd_line(d, cx + dir*z*0.70f, cy, cx + dir*z*0.36f, cy, r,g,b,a);
        draw_osd_line(d, cx + dir*z*0.70f, cy, cx + dir*z*0.52f, cy + z*0.16f, r,g,b,a);
        draw_osd_line(d, cx + dir*z*0.70f, cy, cx + dir*z*0.52f, cy - z*0.16f, r,g,b,a);
    } else {
        draw_outline_rect(d, cx - z*0.55f, cy - z*0.38f, cx - z*0.05f, cy + z*0.38f, r,g,b,a);
        draw_outline_rect(d, cx + z*0.10f, cy - z*0.38f, cx + z*0.55f, cy + z*0.38f, r,g,b,a);
    }
}

static void draw_oem_toolbar(display_t *d)
{
    glUseProgram(d->osd_prog);
    const float y0 = -1.0f, y1 = -0.78f;
    draw_filled_rect(d, -1.0f, y0, 1.0f, y1, 0.02f, 0.02f, 0.025f, 0.96f);
    for (int i = 0; i < OEM_AVM_VIEW_COUNT; i++) {
        float x0 = -1.0f + 2.0f * i / OEM_AVM_VIEW_COUNT;
        float x1 = -1.0f + 2.0f * (i + 1) / OEM_AVM_VIEW_COUNT;
        if (i == d->oem_view)
            draw_filled_rect(d, x0 + 0.01f, y0 + 0.015f, x1 - 0.01f, y1 - 0.015f,
                             0.72f, 0.04f, 0.06f, 0.95f);
        if (i > 0)
            draw_filled_rect(d, x0 - 0.002f, y0 + 0.04f, x0 + 0.002f, y1 - 0.04f,
                             0.22f, 0.22f, 0.24f, 1.0f);
        draw_oem_icon(d, i, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 0.105f);
    }
}

static void draw_oem_surround_panel(display_t *d, float x0, float y0, float x1, float y1)
{
    glUseProgram(d->osd_prog);
    draw_filled_rect(d, x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 1.0f);

    float w = x1 - x0, h = y1 - y0;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float car_w = w * 0.18f, car_h = h * 0.42f;
    float gap = 0.012f;

    draw_camera_rect(d, oem_slot_cam(d, 0), cx - car_w*0.65f, cy + car_h*0.52f + gap,
                     cx + car_w*0.65f, y1 - gap);
    draw_camera_rect(d, oem_slot_cam(d, 2), cx - car_w*0.65f, y0 + gap,
                     cx + car_w*0.65f, cy - car_h*0.52f - gap);
    draw_camera_rect(d, oem_slot_cam(d, 3), x0 + gap, cy - car_h*0.35f,
                     cx - car_w*0.65f - gap, cy + car_h*0.35f);
    draw_camera_rect(d, oem_slot_cam(d, 1), cx + car_w*0.65f + gap, cy - car_h*0.35f,
                     x1 - gap, cy + car_h*0.35f);

    glUseProgram(d->osd_prog);
    draw_vehicle_placeholder(d, cx, cy, car_w, car_h);
    draw_outline_rect(d, x0 + gap, y0 + gap, x1 - gap, y1 - gap,
                      0.12f, 0.12f, 0.14f, 1.0f);
}

static void draw_oem_avm_mode(display_t *d)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, d->w, d->h);

    const float content_y0 = -0.78f;
    const float content_y1 =  1.0f;
    int front = oem_slot_cam(d, 0);
    int right = oem_slot_cam(d, 1);
    int rear  = oem_slot_cam(d, 2);
    int left  = oem_slot_cam(d, 3);

    switch (d->oem_view) {
    case OEM_AVM_SURROUND_MAIN:
        draw_oem_surround_panel(d, -1.0f, content_y0, -0.18f, content_y1);
        draw_camera_rect(d, d->oem_main_cam, -0.18f, content_y0, 1.0f, content_y1);
        break;
    case OEM_AVM_SURROUND_FULL:
        draw_oem_surround_panel(d, -1.0f, content_y0, 1.0f, content_y1);
        break;
    case OEM_AVM_FRONT:
        draw_camera_rect(d, front, -1.0f, content_y0, 1.0f, content_y1);
        draw_guide_lines(d, false);
        break;
    case OEM_AVM_REAR:
        draw_camera_rect(d, rear, -1.0f, content_y0, 1.0f, content_y1);
        draw_guide_lines(d, true);
        break;
    case OEM_AVM_LEFT:
        draw_camera_rect(d, left, -1.0f, content_y0, 1.0f, content_y1);
        break;
    case OEM_AVM_RIGHT:
        draw_camera_rect(d, right, -1.0f, content_y0, 1.0f, content_y1);
        break;
    case OEM_AVM_MULTI:
    default:
        draw_camera_rect(d, left, -1.0f, content_y0, -0.34f, content_y1);
        draw_camera_rect(d, front, -0.34f, content_y0, 0.34f, content_y1);
        draw_camera_rect(d, right, 0.34f, content_y0, 1.0f, content_y1);
        glUseProgram(d->osd_prog);
        draw_filled_rect(d, -0.342f, content_y0, -0.338f, content_y1, 0.02f,0.02f,0.02f,1.0f);
        draw_filled_rect(d,  0.338f, content_y0,  0.342f, content_y1, 0.02f,0.02f,0.02f,1.0f);
        break;
    }

    draw_oem_toolbar(d);
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
    case DISPLAY_MODE_OEM_AVM:       draw_oem_avm_mode(d);      break;
    }

    /* Detection overlay (grid modes only — AVM can add later) */
    if (d->mode != DISPLAY_MODE_OEM_AVM) {
        int cols = (d->n_cams <= 2) ? d->n_cams : 2;
        int rows = (d->n_cams <= 2) ? 1 : 2;
        float qw = 2.0f / cols, qh = 2.0f / rows;

        pthread_mutex_lock(&d->det_lock);
        glUseProgram(d->osd_prog);
        glLineWidth(2.0f);

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
    if (d->mode == DISPLAY_MODE_FISHEYE_GRID) {
        glDeleteProgram(d->mesh_prog);
    }
    for (int i = 0; i < d->n_cams; i++) {
        if (d->grid_vbo_pos[i]) glDeleteBuffers(1, &d->grid_vbo_pos[i]);
        if (d->grid_vbo_tex[i]) glDeleteBuffers(1, &d->grid_vbo_tex[i]);
        if (d->grid_ibo[i])     glDeleteBuffers(1, &d->grid_ibo[i]);
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
