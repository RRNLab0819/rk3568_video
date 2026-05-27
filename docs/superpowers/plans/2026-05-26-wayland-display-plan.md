# Wayland + EGL/GLES2 显示实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 HDMI 上显示 4 路摄像头画面 (2×2 布局), GPU shader 做 YUV→RGB, 不低于 25fps.

**Architecture:** Weston 之上运行 Wayland 全屏客户端。采集线程复用 capture.c, 渲染线程用 EGL/GLES2 创建纹理、运行 YUV→RGB shader、画 4 个 quad, eglSwapBuffers 刷新。

**Tech Stack:** C + Wayland client + EGL + OpenGL ES 2.0 (Mali G52 GPU). 复制原厂 AVM 的 `texture_y_uv.frag` 着色器直接使用。

---

### Task 1: 添加着色器头文件

**Files:**
- Create: `src/shader_yuv.h`

- [ ] **Step 1: 创建着色器源码头文件**

```c
/* src/shader_yuv.h - NV12 YUV→RGB GPU shaders, copied from AVM system */

#ifndef SHADER_YUV_H
#define SHADER_YUV_H

/* Vertex shader: pass-through position + texcoord */
static const char *vert_src =
    "#version 100\n"
    "uniform mat4 u_mvp;\n"
    "attribute vec4 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_pos;\n"
    "  v_tex = a_tex;\n"
    "}\n";

/* Fragment shader: NV12 → RGB (copied from AVM texture_y_uv.frag) */
static const char *frag_src =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform sampler2D u_texY;\n"
    "uniform sampler2D u_texUV;\n"
    "varying vec2 v_tex;\n"
    "const mat3 yuv2rgb = mat3("
    "  1.0, 1.0, 1.0,"
    "  0.0, -0.39465, 2.03211,"
    "  1.13983, -0.58060, 0.0);\n"
    "void main() {\n"
    "  vec3 yuv;\n"
    "  yuv.x = texture2D(u_texY, v_tex).r;\n"
    "  yuv.y = texture2D(u_texUV, v_tex).g - 0.5;\n"
    "  yuv.z = texture2D(u_texUV, v_tex).a - 0.5;\n"
    "  yuv.x -= 0.0625;\n"
    "  gl_FragColor = vec4(yuv2rgb * yuv, 1.0);\n"
    "}\n";

#endif
```

- [ ] **Step 2: Commit**

```bash
git add src/shader_yuv.h
git commit -m "feat: add NV12 YUV→RGB GPU shader from AVM system"
```

---

### Task 2: Wayland + EGL 渲染模块

**Files:**
- Create: `src/render.c`
- Create: `src/render.h`

- [ ] **Step 1: 写 render.h 接口**

```c
/* src/render.h - Wayland + EGL/GLES2 multi-camera display */

#ifndef RENDER_H
#define RENDER_H
#include <stdint.h>

typedef struct render_s render_t;

/* Create fullscreen Wayland window with EGL context.
 * width/height: display resolution (e.g. 1920x1080)
 * n_cameras: number of camera feeds (1-4) */
render_t *render_create(int width, int height, int n_cameras);

/* Update camera texture with new NV12 frame.
 * cam_idx: 0..n_cameras-1
 * nv12_data: pointer to Y plane (UV follows at offset w*h)
 * w, h: frame dimensions */
int render_update_frame(render_t *r, int cam_idx,
                        const uint8_t *nv12_data, int w, int h);

/* Redraw all quads */
void render_draw(render_t *r);

/* Run loop: wait for frame events, call draw_cb each frame */
void render_run(render_t *r);

/* Cleanup */
void render_destroy(render_t *r);

#endif
```

- [ ] **Step 2: 写 render.c — Wayland 连接和 EGL 初始化**

```c
/* src/render.c */
#include "render.h"
#include "shader_yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct render_s {
    int w, h, n_cams;
    /* Wayland */
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct wl_egl_window *egl_window;
    /* EGL */
    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
    EGLSurface egl_surf;
    /* GL */
    GLuint program, loc_mvp, loc_pos, loc_tex;
    GLuint texY[4], texUV[4];
    int frame_w[4], frame_h[4];
    uint8_t *frame_y[4], *frame_uv[4];  /* copies for rendering thread */
};

/* ---- Wayland globals ---- */
static struct wl_compositor *g_compositor;
static struct wl_shell *g_shell;

static void registry_handler(void *d, struct wl_registry *reg,
                              uint32_t id, const char *iface, uint32_t ver)
{
    if (strcmp(iface, "wl_compositor") == 0)
        g_compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (strcmp(iface, "wl_shell") == 0)
        g_shell = wl_registry_bind(reg, id, &wl_shell_interface, 1);
}
static struct wl_registry_listener registry_listener = { registry_handler, NULL };

/* ---- GL helper ---- */
static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[256]; glGetShaderInfoLog(s, 256, NULL, log);
               fprintf(stderr, "[render] shader err: %s\n", log); }
    return s;
}

render_t *render_create(int w, int h, int n_cams)
{
    render_t *r = calloc(1, sizeof(*r));
    r->w = w; r->h = h; r->n_cams = (n_cams > 4) ? 4 : n_cams;

    /* 1. Wayland connection */
    r->display = wl_display_connect(NULL);
    if (!r->display) { perror("wl_display_connect"); goto fail; }
    struct wl_registry *reg = wl_display_get_registry(r->display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(r->display);
    if (!g_compositor || !g_shell) { fprintf(stderr, "no compositor\n"); goto fail; }

    /* 2. Surface */
    r->surface = wl_compositor_create_surface(g_compositor);
    r->shell_surface = wl_shell_get_shell_surface(g_shell, r->surface);
    wl_shell_surface_set_fullscreen(r->shell_surface,
                                     WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
                                     0, NULL);
    wl_shell_surface_set_toplevel(r->shell_surface);

    /* 3. EGL window */
    r->egl_window = wl_egl_window_create(r->surface, w, h);
    if (!r->egl_window) { fprintf(stderr, "wl_egl_window_create failed\n"); goto fail; }

    /* 4. EGL context */
    r->egl_dpy = eglGetDisplay((EGLNativeDisplayType)r->display);
    eglInitialize(r->egl_dpy, NULL, NULL);
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8,
                          EGL_NONE };
    EGLConfig cfg; EGLint n;
    eglChooseConfig(r->egl_dpy, cfg_attr, &cfg, 1, &n);
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    r->egl_ctx = eglCreateContext(r->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    r->egl_surf = eglCreateWindowSurface(r->egl_dpy, cfg,
                                          (EGLNativeWindowType)r->egl_window, NULL);
    eglMakeCurrent(r->egl_dpy, r->egl_surf, r->egl_surf, r->egl_ctx);

    /* 5. Compile shader */
    r->program = glCreateProgram();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    glAttachShader(r->program, vs); glAttachShader(r->program, fs);
    glLinkProgram(r->program);
    glDeleteShader(vs); glDeleteShader(fs);
    r->loc_mvp = glGetUniformLocation(r->program, "u_mvp");
    r->loc_pos = glGetAttribLocation(r->program, "a_pos");
    r->loc_tex = glGetAttribLocation(r->program, "a_tex");

    /* 6. Init textures & frame buffers */
    for (int i = 0; i < r->n_cams; i++) {
        glGenTextures(1, &r->texY[i]);
        glGenTextures(1, &r->texUV[i]);
        r->frame_y[i] = NULL;
        r->frame_uv[i] = NULL;
    }

    /* 7. Initial draw to show something */
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(r->egl_dpy, r->egl_surf);

    printf("[render] Wayland+EGL %dx%d, %d cameras\n", w, h, n_cams);
    return r;

fail:
    free(r);
    return NULL;
}
```

- [ ] **Step 3: 写 render_update_frame — 更新相机帧 (拷贝 + 纹理上传)**

```c
int render_update_frame(render_t *r, int cam_idx,
                        const uint8_t *nv12, int w, int h)
{
    if (cam_idx < 0 || cam_idx >= r->n_cams || !nv12) return -1;

    /* Copy Y plane */
    size_t ysz = w * h;
    free(r->frame_y[cam_idx]);
    r->frame_y[cam_idx] = malloc(ysz);
    memcpy(r->frame_y[cam_idx], nv12, ysz);

    /* Copy UV plane */
    size_t uvsz = ysz / 2;
    free(r->frame_uv[cam_idx]);
    r->frame_uv[cam_idx] = malloc(uvsz);
    memcpy(r->frame_uv[cam_idx], nv12 + ysz, uvsz);

    r->frame_w[cam_idx] = w;
    r->frame_h[cam_idx] = h;
    return 0;
}
```

- [ ] **Step 4: 写 render_draw — 渲染 2×2 布局**

```c
void render_draw(render_t *r)
{
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(r->program);
    glViewport(0, 0, r->w, r->h);

    int cols = 2, rows = (r->n_cams <= 2) ? 1 : 2;
    float quad_w = 2.0f / cols;
    float quad_h = 2.0f / rows;

    for (int i = 0; i < r->n_cams; i++) {
        if (!r->frame_y[i] || !r->frame_uv[i]) continue;

        int col = i % cols, row = i / cols;
        float x0 = -1.0f + col * quad_w;
        float y0 = -1.0f + row * quad_h;
        float x1 = x0 + quad_w, y1 = y0 + quad_h;

        /* Upload Y texture */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r->texY[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                     r->frame_w[i], r->frame_h[i], 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, r->frame_y[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        /* Upload UV texture (half resolution) */
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, r->texUV[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                     r->frame_w[i]/2, r->frame_h[i]/2, 0,
                     GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, r->frame_uv[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        /* Set texture units */
        glUniform1i(glGetUniformLocation(r->program, "u_texY"), 0);
        glUniform1i(glGetUniformLocation(r->program, "u_texUV"), 1);

        /* MVP matrix for this quad */
        float mvp[16] = { quad_w/2,0,0,0, 0,quad_h/2,0,0, 0,0,1,0,
                          x0+quad_w/2, y0+quad_h/2,0,1 };
        glUniformMatrix4fv(r->loc_mvp, 1, GL_FALSE, mvp);

        /* Quad vertices (-1..1 in local coords) */
        float verts[] = { -1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,1,0,1 };
        glVertexAttribPointer(r->loc_pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
        glVertexAttribPointer(r->loc_tex, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
        glEnableVertexAttribArray(r->loc_pos);
        glEnableVertexAttribArray(r->loc_tex);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(r->loc_pos);
        glDisableVertexAttribArray(r->loc_tex);
    }

    eglSwapBuffers(r->egl_dpy, r->egl_surf);
}
```

- [ ] **Step 5: 写 render_run 和 render_destroy**

```c
void render_run(render_t *r)
{
    /* Dispatch Wayland events */
    wl_display_dispatch_pending(r->display);
}

void render_destroy(render_t *r)
{
    if (!r) return;
    for (int i = 0; i < r->n_cams; i++) {
        free(r->frame_y[i]); free(r->frame_uv[i]);
    }
    if (r->egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(r->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (r->egl_surf) eglDestroySurface(r->egl_dpy, r->egl_surf);
        if (r->egl_ctx) eglDestroyContext(r->egl_dpy, r->egl_ctx);
        eglTerminate(r->egl_dpy);
    }
    if (r->egl_window) wl_egl_window_destroy(r->egl_window);
    if (r->shell_surface) wl_shell_surface_destroy(r->shell_surface);
    if (r->surface) wl_surface_destroy(r->surface);
    if (r->display) wl_display_disconnect(r->display);
    free(r);
}
```

- [ ] **Step 6: Commit**

```bash
git add src/render.c src/render.h
git commit -m "feat: Wayland+EGL/GLES2 render module with GPU YUV→RGB shader"
```

---

### Task 3: 集成渲染到流水线

**Files:**
- Modify: `src/common.h` (添加 render 函数指针回调)
- Modify: `src/main.c` (Wayland 显示替代 GStreamer)
- Modify: `Makefile` (编译 render.c, 链接 wayland/egl)

- [ ] **Step 1: 更新 Makefile 编译 render.c**

修改 Makefile, 不需要改 SOURCES (wildcard 自动包含 render.c), 只需加链接库:

```makefile
# 已有的 LIBS 行后加 wayland/egl
LIBS    := -lrockchip_mpp -lrknnrt -lrga -ldrm -lpthread -lrt -ldl -lm
LIBS    += -lwayland-client -lwayland-egl -lEGL -lGLESv2
```

- [ ] **Step 2: 改写 main.c 的 --display 路径**

移除 GStreamer kmssink 的 system() 调用, 替换为 Wayland render:

```c
/* src/main.c - 在 pipeline 启动后再启动 render 循环 */

/* 在 main() 中, pipeline_start 之后: */

render_t *render = NULL;
if (use_display) {
    printf("[main] starting Wayland display...\n");
    render = render_create(width, height, n_cameras);
}

printf("[main] Running. %s\n", max_frames > 0 ? "Will exit" : "Ctrl+C to stop");

struct timeval last_draw = {0};
while (g_pipeline) {
    /* Check completion */
    if (max_frames > 0 && pipeline_is_done(g_pipeline)) {
        printf("[main] frames done, stopping\n");
        break;
    }

    /* Wayland event loop (non-blocking) */
    if (render) {
        render_run(render);

        /* Redraw at ~30fps */
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - last_draw.tv_sec) * 1000000
                     + (now.tv_usec - last_draw.tv_usec);
        if (elapsed >= 33000) { /* 33ms = 30fps */
            render_draw(render);
            last_draw = now;
        }
    } else {
        sleep(1);
    }
}

/* Cleanup */
if (render) render_destroy(render);
```

- [ ] **Step 3: 通过回调传递帧数据到 render**

修改 pipeline 的回调函数, 把 NV12 帧传给 render:

```c
/* 在 main.c 的 encoded_frame_cb 中添加: */
static void on_encoded_frame(int channel, const uint8_t *data, size_t len,
                             bool keyframe, int64_t pts, void *userdata)
{
    /* ...existing file write code... */

    /* Feed to renderer (userdata = render_t*) */
    render_t *r = (render_t *)userdata;
    /* NOTE: encoded_frame_cb only gets encoded data, not NV12.
     * We need a SEPARATE callback for raw frames. */
}
```

**修正**: pipeline 需要添加一个 raw frame callback, 在编码之前/同时把 NV12 数据传给 render:

```c
/* 在 pipeline.h 中添加: */
typedef void (*raw_frame_cb)(int channel, const uint8_t *nv12_data,
                             int width, int height, void *userdata);
void pipeline_set_raw_callback(pipeline_t *p, raw_frame_cb cb, void *ud);
```

```c
/* 在 pipeline.c 的 camera_thread 中, 编码之前添加: */
if (ch->pipeline->raw_cb) {
    ch->pipeline->raw_cb(ch->id, buf->ptr,
                         buf->width, buf->height,
                         ch->pipeline->raw_cb_data);
}
```

```c
/* 在 main.c 中设置回调: */
static void on_raw_frame(int channel, const uint8_t *nv12,
                         int w, int h, void *userdata)
{
    render_t *r = (render_t *)userdata;
    render_update_frame(r, channel, nv12, w, h);
}

/* 在 main() 的 pipeline 初始化后: */
if (render) {
    pipeline_set_raw_callback(g_pipeline, on_raw_frame, render);
}
```

- [ ] **Step 4: Commit**

```bash
git add src/main.c src/pipeline.c src/pipeline.h src/common.h Makefile
git commit -m "feat: integrate Wayland render into pipeline with raw frame callback"
```

---

### Task 4: 交叉编译和部署测试

- [ ] **Step 1: 编译**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make clean && make
```

预期: 编译成功, 生成 `rk3568_camera` 二进制, 链接 `libwayland-client`, `libwayland-egl`, `libEGL`, `libGLESv2`

- [ ] **Step 2: 部署**

```bash
adb push rk3568_camera /userdata/rk3568_camera
adb push config.ini /userdata/rk3568-camera/config.ini
```

- [ ] **Step 3: 测试**

```bash
# Weston 必须在运行 (不要 killall -STOP!)
adb shell "ps aux | grep weston"  # 确认 Weston 在跑

# 测试 1 路摄像头 + 显示
adb shell "/userdata/rk3568_camera -c 1 --display -n 100"
```

预期输出:
```
[render] Wayland+EGL 1920x1080, 1 cameras
[ch0] 25 frames in 1.0s = 24.x fps
```

预期效果: HDMI 屏幕显示摄像头画面 (单路全屏)

- [ ] **Step 4: 测试 4 路 2×2 布局**

```bash
adb shell "/userdata/rk3568_camera -c 4 --display -n 100"
```

预期: HDMI 显示 2×2 四画面

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build: add Wayland/EGL link flags"
```
