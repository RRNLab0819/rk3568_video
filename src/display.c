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
 * Render the current frame to the window.
 *
 * Layout: cameras arranged in a 2x2 grid.  Missing cameras (no frame
 * received yet) are silently skipped.
 */
void disp_draw(display_t *d)
{
    if (!d)
        return;

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(d->program);
    glViewport(0, 0, d->w, d->h);

    int cols = (d->n_cams <= 2) ? d->n_cams : 2;
    int rows = (d->n_cams <= 2) ? 1 : 2;
    float qw = 2.0f / cols;   /* quad width in clip space */
    float qh = 2.0f / rows;   /* quad height in clip space */

    GLint u_texY  = glGetUniformLocation(d->program, "u_texY");
    GLint u_texUV = glGetUniformLocation(d->program, "u_texUV");

    for (int i = 0; i < d->n_cams; i++) {
        if (!d->has_frame[i])
            continue;

        int col = i % cols;
        int row = i / cols;

        /* Quad in clip space: x,y ∈ [-1, 1], row 0 = top */
        float x0 = -1.0f + col * qw;
        float x1 = x0 + qw;
        float y1 =  1.0f - row * qh;       /* top edge */
        float y0 = y1 - qh;                /* bottom edge */

        /* Bind the textures that were populated by import_nv12 */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->texY[i]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, d->texUV[i]);

        glUniform1i(u_texY, 0);
        glUniform1i(u_texUV, 1);

        /* Quad vertices interleaved: [pos2, tex2] */
        float verts[] = {
            x0, y0,  0.0f, 0.0f,   /* bottom-left  */
            x1, y0,  1.0f, 0.0f,   /* bottom-right */
            x1, y1,  1.0f, 1.0f,   /* top-right    */
            x0, y1,  0.0f, 1.0f,   /* top-left     */
        };
        glVertexAttribPointer(d->loc_pos, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), verts);
        glVertexAttribPointer(d->loc_tex, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), verts + 2);
        glEnableVertexAttribArray(d->loc_pos);
        glEnableVertexAttribArray(d->loc_tex);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(d->loc_pos);
        glDisableVertexAttribArray(d->loc_tex);
    }

    /* Draw detection boxes per channel in each tile */
    pthread_mutex_lock(&d->det_lock);
    glUseProgram(d->osd_prog);
    glLineWidth(3.0f);

    static const float ch_colors[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f},  /* ch0: red */
        {0.0f, 1.0f, 0.0f, 1.0f},  /* ch1: green */
        {0.0f, 0.0f, 1.0f, 1.0f},  /* ch2: blue */
        {1.0f, 1.0f, 0.0f, 1.0f},  /* ch3: yellow */
    };

    for (int cam = 0; cam < d->n_cams && cam < 4; cam++) {
        if (!d->has_frame[cam]) continue;
        if (d->det_count[cam] <= 0) continue;

        int col = cam % cols, row = cam / cols;
        float tx0 = -1.0f + col * qw;
        float ty1 =  1.0f - row * qh;
        float xs = qw / 1920.0f;
        float ys = qh / 1080.0f;

        glUniform4fv(d->osd_color_loc, 1, ch_colors[cam]);

        for (int i = 0; i < d->det_count[cam]; i++) {
            detection_t *dt = &d->dets[cam][i];
            if (dt->class_id != 0) continue;  /* person only */
            float bx = tx0 + dt->x * xs;
            float by = ty1 - (dt->y + dt->h) * ys;
            float bw = dt->w * xs;
            float bh = dt->h * ys;
            float verts[] = { bx,by, bx+bw,by, bx+bw,by+bh, bx,by+bh };
            glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
            glEnableVertexAttribArray(d->osd_pos);
            glDrawArrays(GL_LINE_LOOP, 0, 4);
            glDisableVertexAttribArray(d->osd_pos);
        }
    }
    pthread_mutex_unlock(&d->det_lock);

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
