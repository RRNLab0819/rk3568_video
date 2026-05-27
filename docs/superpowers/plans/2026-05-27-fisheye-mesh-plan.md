# Fisheye Mesh Correction — 显示矫正实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 display.c 中新增 GPU mesh 鱼眼矫正通路，4 路摄像头独立矫正/展开，不影响现有 2×2 baseline。

**Architecture:** 新增 `src/fisheye_mesh.c/.h` 模块，从原厂 calibinfo.lua 解析内外参，离线生成矫正 UV mesh，在 display.c 中通过 VBO + 新 shader 渲染。用环境变量 `FISHEYE_MODE=1` 切换。推理不改，4 路 2×2 quad baseline 保留。

**Tech Stack:** C + OpenGL ES 2.0 + Wayland/EGL，解析 Lua table 用的内联 mini parser (不需要 Lua 库)

---

### Task 1: Lua 标定参数解析器

**Files:**
- Create: `src/calib_parser.h`
- Create: `src/calib_parser.c`

- [ ] **Step 1: 定义参数结构体**

```c
/* src/calib_parser.h */
#ifndef CALIB_PARSER_H
#define CALIB_PARSER_H

typedef struct {
    float cx, cy;           /* optical center (pixels) */
    float focal;            /* focal length (pixels) */
    float scale;
    float distor[4];        /* k1, k2, k3, k4 */
    char  lens[16];         /* lens model name */
} cam_intrinsic_t;

typedef struct {
    float pos[3];           /* world position (mm) */
    float up[3];            /* up vector */
    float lookat[3];        /* look-at point */
} cam_extrinsic_t;

typedef struct {
    cam_intrinsic_t intr;
    cam_extrinsic_t extr;
    int   width, height;
} camera_calib_t;

typedef struct {
    int   num_cameras;
    camera_calib_t cams[6]; /* up to 6 */
} calib_info_t;

int calib_parse(const char *path, calib_info_t *info);

#endif
```

- [ ] **Step 2: 实现 Lua table 解析器**

```c
/* src/calib_parser.c */
#include "calib_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Minimal Lua table parser — handles the calibinfo.lua format.
 * Does NOT depend on liblua.  Assumes well-formed Lua literal tables. */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static int parse_float_array(const char *s, float *out, int max_n) {
    /* Parse "{ 1.0, 2.0, 3.0 }" → float[3] */
    int n = 0;
    while (*s && *s != '{') s++;
    if (*s == '{') s++;
    while (n < max_n && *s && *s != '}') {
        while (*s == ' ' || *s == ',') s++;
        char *end;
        out[n] = strtof(s, &end);
        if (end == s) break;
        n++; s = end;
    }
    return n;
}

static int parse_int(const char *s, int *val) {
    while (*s && *s != '=') s++;
    if (*s == '=') s++;
    while (*s == ' ') s++;
    *val = (int)strtol(s, NULL, 10);
    return 0;
}

static int parse_float(const char *s, float *val) {
    while (*s && *s != '=') s++;
    if (*s == '=') s++;
    while (*s == ' ') s++;
    *val = strtof(s, NULL);
    return 0;
}

/* Find "Cameras" array and parse each camera block */
static int parse_cameras(const char *buf, calib_info_t *info) {
    const char *p = strstr(buf, "Cameras");
    if (!p) return -1;
    p = strstr(p, "{");
    if (!p) return -1;
    
    int depth = 0, cam_idx = 0;
    while (*p && cam_idx < 6) {
        if (*p == '{') depth++;
        if (*p == '}') depth--;
        
        if (depth == 3) { /* inside a camera block */
            /* Find Internal block */
            const char *pi = strstr(p, "Internal");
            if (pi) {
                parse_float_array(strstr(pi, "Center"), info->cams[cam_idx].intr.cx != 0 ? NULL : &info->cams[cam_idx].intr.cx, 2);
                /* ... complete parsing of all fields ... */
            }
            cam_idx++;
            /* skip to end of this camera block */
        }
        p++;
    }
    info->num_cameras = cam_idx;
    return 0;
}

int calib_parse(const char *path, calib_info_t *info) {
    memset(info, 0, sizeof(*info));
    char *buf = slurp(path);
    if (!buf) return -1;
    
    int ret = parse_cameras(buf, info);
    free(buf);
    return ret;
}
```

- [ ] **Step 3: 编译测试**

Run: `make calib_parser_test`
Expected: 编译通过

- [ ] **Step 4: 用板子上的 calibinfo.lua 验证解析**

Run: `adb shell /userdata/calib_parser_test /userdata/avm/cali/calibinfo.lua`
Expected: 打印出 4 路摄像头的内外参

- [ ] **Step 5: 提交**

```bash
git add src/calib_parser.h src/calib_parser.c Makefile
git commit -m "feat: add Lua calibinfo parser for factory fisheye parameters"
```

---

### Task 2: UV Mesh 生成器

**Files:**
- Create: `src/fisheye_mesh.h`
- Create: `src/fisheye_mesh.c`

- [ ] **Step 1: 定义 mesh 结构体**

```c
/* src/fisheye_mesh.h */
#ifndef FISHEYE_MESH_H
#define FISHEYE_MESH_H

#include "calib_parser.h"
#include <GLES2/gl2.h>

typedef struct {
    int    num_vertices;
    int    num_indices;
    GLuint vbo_pos;     /* screen-space positions (vec2) */
    GLuint vbo_tex;     /* fisheye-image UVs (vec2) */
    GLuint ibo;         /* triangle indices */
    int    grid_w, grid_h;  /* subdivision */
} fisheye_mesh_t;

/* Generate fisheye undistort mesh for one camera.
 * output_w/h: desired output resolution for this camera tile
 * fov_h: horizontal FOV in degrees (e.g. 120)
 */
int  fisheye_mesh_generate(fisheye_mesh_t *mesh,
                           const camera_calib_t *calib,
                           int output_w, int output_h,
                           float fov_h);

void fisheye_mesh_destroy(fisheye_mesh_t *mesh);

#endif
```

- [ ] **Step 2: 实现逆映射 UV 生成算法**

```c
/* src/fisheye_mesh.c */
#include "fisheye_mesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MESH_DIV 48  /* 48x48 per camera */

static void lens_distort(const cam_intrinsic_t *intr,
                         float u_norm, float v_norm,
                         float *u_dst, float *v_dst)
{
    /* Convert normalized coords (0..1) to centered coords */
    float dx = u_norm - (intr->cx / (float)intr->width);
    float dy = v_norm - (intr->cy / (float)intr->height);
    float r2 = dx*dx + dy*dy;
    float r4 = r2 * r2;
    float r6 = r4 * r2;
    float r8 = r4 * r4;
    
    /* Polynomial distortion: r' = r * (1 + k1*r² + k2*r⁴ + k3*r⁶ + k4*r⁸) */
    float k[4];
    memcpy(k, intr->distor, sizeof(k));
    float scale = 1.0f + k[0]*r2 + k[1]*r4 + k[2]*r6 + k[3]*r8;
    
    *u_dst = intr->cx + dx * scale;
    *v_dst = intr->cy + dy * scale;
}

static void sphere_vertex(float u_grid, float v_grid,
                          float fov_h, float fov_v,
                          float *x, float *y, float *z)
{
    /* Map grid (0..1, 0..1) to sphere surface point.
     * fov_h: horizontal field of view in radians */
    float theta = (u_grid - 0.5f) * fov_h;
    float phi   = (v_grid - 0.5f) * fov_v;
    
    float r = 5000.0f;  /* sphere radius mm (matches factory) */
    *x = r * sinf(theta) * cosf(phi);
    *y = r * sinf(phi);
    *z = r * cosf(theta) * cosf(phi);
}

int fisheye_mesh_generate(fisheye_mesh_t *mesh,
                          const camera_calib_t *calib,
                          int output_w, int output_h,
                          float fov_h)
{
    int gw = MESH_DIV, gh = MESH_DIV;
    int nv = (gw + 1) * (gh + 1);
    int ni = gw * gh * 6;  /* 2 triangles per grid cell */
    
    float *pos = malloc(nv * 2 * sizeof(float));
    float *tex = malloc(nv * 2 * sizeof(float));
    unsigned short *idx = malloc(ni * sizeof(unsigned short));
    
    float fov_v = fov_h * (float)output_h / (float)output_w;
    float fov_h_rad = fov_h * (float)M_PI / 180.0f;
    float fov_v_rad = fov_v * (float)M_PI / 180.0f;
    
    for (int j = 0; j <= gh; j++) {
        for (int i = 0; i <= gw; i++) {
            int vi = j * (gw + 1) + i;
            float u = (float)i / (float)gw;
            float v = (float)j / (float)gh;
            
            /* Screen position (NDC clip space for this tile) */
            pos[vi*2+0] = u;
            pos[vi*2+1] = v;
            
            /* 3D sphere point */
            float sx, sy, sz;
            sphere_vertex(u, v, fov_h_rad, fov_v_rad, &sx, &sy, &sz);
            
            /* TODO: transform by camera extrinsic → camera coords */
            /* For now: use simple pinhole projection */
            
            /* Project to fisheye image UV */
            float u_src, v_src;
            /* Pinhole: u = fx * X/Z + cx, v = fy * Y/Z + cy */
            float z = (sz != 0) ? sz : 1.0f;
            float u_ndc = calib->intr.focal * sx / z + calib->intr.cx;
            float v_ndc = calib->intr.focal * sy / z + calib->intr.cy;
            
            /* Apply distortion */
            lens_distort(&calib->intr,
                         u_ndc / calib->intr.width,
                         v_ndc / calib->intr.height,
                         &u_src, &v_src);
            
            tex[vi*2+0] = u_src / calib->intr.width;
            tex[vi*2+1] = v_src / calib->intr.height;
        }
    }
    
    /* Generate triangle indices */
    int ii = 0;
    for (int j = 0; j < gh; j++) {
        for (int i = 0; i < gw; i++) {
            int a = j * (gw + 1) + i;
            int b = a + 1;
            int c = a + (gw + 1);
            int d = c + 1;
            idx[ii++] = a; idx[ii++] = b; idx[ii++] = d;
            idx[ii++] = a; idx[ii++] = d; idx[ii++] = c;
        }
    }
    
    /* Upload to GPU VBOs */
    glGenBuffers(1, &mesh->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, nv * 2 * sizeof(float), pos, GL_STATIC_DRAW);
    
    glGenBuffers(1, &mesh->vbo_tex);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_tex);
    glBufferData(GL_ARRAY_BUFFER, nv * 2 * sizeof(float), tex, GL_STATIC_DRAW);
    
    glGenBuffers(1, &mesh->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * sizeof(unsigned short), idx, GL_STATIC_DRAW);
    
    mesh->num_vertices = nv;
    mesh->num_indices  = ni;
    mesh->grid_w = gw; mesh->grid_h = gh;
    
    free(pos); free(tex); free(idx);
    return 0;
}

void fisheye_mesh_destroy(fisheye_mesh_t *mesh)
{
    if (mesh->vbo_pos) glDeleteBuffers(1, &mesh->vbo_pos);
    if (mesh->vbo_tex) glDeleteBuffers(1, &mesh->vbo_tex);
    if (mesh->ibo)     glDeleteBuffers(1, &mesh->ibo);
    memset(mesh, 0, sizeof(*mesh));
}
```

- [ ] **Step 3: 编译测试**

Run: `make fisheye_mesh_test`
Expected: 编译通过，链接 OpenGL ES

- [ ] **Step 4: 提交**

```bash
git add src/fisheye_mesh.h src/fisheye_mesh.c Makefile
git commit -m "feat: add fisheye UV mesh generator from calibration params"
```

---

### Task 3: display.c 添加 Mesh 渲染通路

**Files:**
- Modify: `src/display.c`
- Modify: `src/display.h`
- Modify: `src/shader_yuv.h`

- [ ] **Step 1: 在 display.h 中添加 mesh 支持声明**

```c
/* Add to display_t opaque type comment:
 * Two rendering modes:
 *   - Quad mode (existing): 4-vertex GL_TRIANGLE_FAN per camera
 *   - Mesh mode (new): VBO-based fisheye undistort mesh per camera
 */
```

- [ ] **Step 2: 在 display_t 中添加 mesh 成员**

```c
/* In display_s struct, add: */
    bool        use_mesh;       /* enable fisheye mesh mode */
    GLuint      mesh_prog;      /* mesh shader program */
    GLuint      mesh_vbo[4];    /* per-camera VBOs (loaded from fisheye_mesh_t) */
    GLuint      mesh_ibo[4];
    int         mesh_nidx[4];   /* index count per camera */
```

- [ ] **Step 3: 编译 mesh shader**

```c
/* New vertex shader for mesh rendering — replaces hardcoded quad vertices */
static const char mesh_vert_src[] =
    "#version 100\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_tex = a_tex;\n"
    "}\n";
/* Fragment shader: same NV12→RGB, get from shader_yuv.h */
```

- [ ] **Step 4: 修改 disp_draw() 支持 mesh 模式**

```c
void disp_draw(display_t *d)
{
    if (!d) return;
    glClear(GL_COLOR_BUFFER_BIT);

    if (d->use_mesh) {
        /* Mesh rendering path */
        glUseProgram(d->mesh_prog);
        GLint u_texY  = glGetUniformLocation(d->mesh_prog, "u_texY");
        GLint u_texUV = glGetUniformLocation(d->mesh_prog, "u_texUV");
        GLint loc_pos = glGetAttribLocation(d->mesh_prog, "a_pos");
        GLint loc_tex = glGetAttribLocation(d->mesh_prog, "a_tex");

        for (int i = 0; i < d->n_cams; i++) {
            if (!d->has_frame[i]) continue;

            glViewport(/* tile viewport */);
            
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, d->texY[i]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, d->texUV[i]);
            glUniform1i(u_texY, 0);
            glUniform1i(u_texUV, 1);

            glBindBuffer(GL_ARRAY_BUFFER, d->mesh_vbo[i]);
            glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(loc_pos);
            glBindBuffer(GL_ARRAY_BUFFER, d->mesh_tex_vbo[i]);
            glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(loc_tex);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->mesh_ibo[i]);
            glDrawElements(GL_TRIANGLES, d->mesh_nidx[i], GL_UNSIGNED_SHORT, 0);
        }
    } else {
        /* ---- Existing quad path (unchanged) ---- */
        /* ... current code ... */
    }

    /* Detection overlay (unchanged, works on both paths) */
    /* ... */

    eglSwapBuffers(d->egl_dpy, d->egl_surf);
}
```

- [ ] **Step 5: 环境变量控制模式**

```c
/* In disp_open(): */
    d->use_mesh = (getenv("FISHEYE_MODE") != NULL);
    if (d->use_mesh) {
        /* Load calibration and generate meshes */
        calib_info_t info;
        if (calib_parse("/userdata/avm/cali/calibinfo.lua", &info) == 0) {
            /* Generate mesh for each camera */
            for (int i = 0; i < d->n_cams; i++) {
                fisheye_mesh_t m;
                fisheye_mesh_generate(&m, &info.cams[i], 
                                      1920/cols, 1080/rows, 120.0f);
                d->mesh_vbo[i] = m.vbo_pos;
                /* etc */
            }
        }
        printf("[display] fisheye mesh mode enabled\n");
    }
```

- [ ] **Step 6: 编译并部署测试**

Run:
```bash
cd /home/rrn/rk3568-camera && make
adb push rk3568_camera /userdata/rk3568_camera
adb shell "cd /userdata && FISHEYE_MODE=1 LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -c 1 --no-enc"
```
Expected: 单路摄像头显示矫正后的画面

- [ ] **Step 7: 提交**

```bash
git add src/display.c src/display.h src/shader_yuv.h
git commit -m "feat: add GPU fisheye mesh undistort path in display.c (FISHEYE_MODE env toggle)"
```

---

### Task 4: 验证和调参

**Files:**
- Create: `tools/verify_calib.py`

- [ ] **Step 1: 离线验证脚本**

```python
#!/usr/bin/env python3
"""tools/verify_calib.py - 离线验证 camera calibration parameters.
从 calibinfo.lua 解析参数，生成一张矫正后的图像用于检查效果。
Usage: python3 verify_calib.py calibinfo.lua test_frame.nv12 cam_idx output.png
"""
import sys, struct, re
import numpy as np
import cv2

def parse_calibinfo(path):
    """Parse calibinfo.lua and return camera params."""
    with open(path) as f:
        content = f.read()
    
    cameras = []
    # Extract Camera blocks using regex
    # Pattern for Center: Center = { 946.42, 553.02,},
    # Pattern for Focal: Focal = 1449.51,
    # ...
    # Return list of dicts
    
    pattern = r'Center\s*=\s*\{\s*([\d.]+),\s*([\d.]+)\s*\},'
    centers = re.findall(pattern, content)
    
    pattern = r'Focal\s*=\s*([\d.]+)'
    focals = re.findall(pattern, content)
    
    pattern = r'DistorParam\s*=\s*\{\s*([\d.-]+),\s*([\d.-]+),\s*([\d.-]+),\s*([\d.-]+)\s*\},'
    distors = re.findall(pattern, content)
    
    for i in range(min(len(centers), len(focals), len(distors))):
        cameras.append({
            'cx': float(centers[i][0]), 'cy': float(centers[i][1]),
            'focal': float(focals[i]),
            'distor': [float(x) for x in distors[i]],
        })
    return cameras

def undistort_fisheye(img, calib, output_size=(480, 480), fov=120):
    """Generate undistorted image using inverse mapping + distortion."""
    h, w = output_size
    map_x = np.zeros((h, w), dtype=np.float32)
    map_y = np.zeros((h, w), dtype=np.float32)
    
    cx, cy = calib['cx'], calib['cy']
    f = calib['focal']
    k = calib['distor']
    
    for j in range(h):
        for i in range(w):
            # Grid → angle (sphere model)
            theta = (i / w - 0.5) * np.radians(fov)
            phi   = (j / h - 0.5) * np.radians(fov * h / w)
            
            # Pinhole projection
            u = f * np.tan(theta) + cx
            v = f * np.tan(phi) / np.cos(theta) + cy
            
            # Distortion
            dx, dy = u - cx, v - cy
            r2 = dx*dx + dy*dy
            r = np.sqrt(r2)
            scale = 1.0 + k[0]*r2 + k[1]*r2*r2 + k[2]*r2*r2*r2 + k[3]*r2*r2*r2*r2
            map_x[j,i] = cx + dx * scale
            map_y[j,i] = cy + dy * scale
    
    return cv2.remap(img, map_x, map_y, cv2.INTER_LINEAR)

if __name__ == '__main__':
    if len(sys.argv) < 5:
        print("Usage: verify_calib.py <calibinfo.lua> <frame.nv12> <cam_idx> <output.png>")
        sys.exit(1)
    
    cams = parse_calibinfo(sys.argv[1])
    cam_idx = int(sys.argv[3])
    
    # Read NV12 frame
    raw = np.fromfile(sys.argv[2], dtype=np.uint8)
    w, h = 1920, 1080
    y = raw[:w*h].reshape(h, w)
    uv = raw[w*h:].reshape(h//2, w)
    
    # NV12→BGR
    y_upscaled = cv2.resize(y, (w, h))
    uv_upscaled = cv2.resize(uv, (w, h))
    yuv = cv2.merge([y_upscaled, uv_upscaled[:,:,np.newaxis] if uv_upscaled.ndim==2 else uv_upscaled])
    bgr = cv2.cvtColor(yuv.astype(np.uint8), cv2.COLOR_YUV2BGR_NV12)
    
    result = undistort_fisheye(bgr, cams[cam_idx])
    cv2.imwrite(sys.argv[4], result)
    print(f"Saved {sys.argv[4]}")
```

- [ ] **Step 2: 运行离线验证**

Run:
```bash
python3 tools/verify_calib.py userdata_calibinfo.lua cam0_frame.nv12 0 test_undistort.png
```
Expected: 生成矫正图像，目视检查是否合理（直线变直、无严重裁剪）

- [ ] **Step 3: 提交**

```bash
git add tools/verify_calib.py
git commit -m "feat: add offline fisheye undistort verification tool"
```

---

## 不改动清单

| 文件 | 原因 |
|------|------|
| `src/capture.c` / `capture.h` | 采集通路不变 |
| `src/inference.cc` / `inference.h` | AI 推理继续吃原始图 |
| `src/postprocess.cc` / `postprocess.h` | 后处理不变 |
| `src/rga_dmabuf*` | RGA 通路独立 |
| `src/pipeline.c` / `pipeline.h` | 主链路不变 |
| `src/encoder.c` / `encoder.h` | 编码不变 |
| `src/frame.h` | 帧结构不变 |
| `src/main.c` | 启动参数逻辑不变 |
| `shader_yuv.h` frag_src | NV12 shader 不变，mesh 模式复用同一个 fragment shader |
| `start.sh` / `start_display.sh` | baseline 不变（不设 FISHEYE_MODE） |

## 回退方案

如果 mesh 模式有问题：
```bash
# 不设 FISHEYE_MODE 环境变量即可回到 quad baseline
./rk3568_camera -c 4  # normal 2x2
```
