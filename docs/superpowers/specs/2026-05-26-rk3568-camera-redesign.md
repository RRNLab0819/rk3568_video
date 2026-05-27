# RK3568 4路 AI 安防摄像头 — 重设计方案

## 目标

4路 1080p@25fps 同步采集 + H.265 编码 + HDMI 2×2 画面显示 + 可选 AI 检测。全链路 dma_buf 零拷贝，走瑞芯微官方技术路线。

## 架构图

```
┌──────────────────────────────────────────────────────────────┐
│                         RK3568                               │
│                                                              │
│  TP9930 (4ch AHD→DVP)                                        │
│    │                                                         │
│    ├─ /dev/video0 ── capture[0] ─┬─ ring[0]_disp ─────────┐ │
│    ├─ /dev/video1 ── capture[1] ─┤                        │ │
│    ├─ /dev/video2 ── capture[2] ─┤    frame_t (dma_buf fd) │ │
│    └─ /dev/video3 ── capture[3] ─┤                        │ │
│                                   │                        │ │
│     每路采集线程 V4L2 dma_buf     │                        │ │
│                                   │                        ▼ │
│     编码在采集线程内同步:          │              render thread │
│       MPP import dma_buf         │    Wayland+EGL+GLES2    │
│         → H.265 .h264 文件        │   eglCreateImageKHR    │
│                                   │     (DMA_BUF_EXT)      │
│     可选推理:                      │   → GL texture          │
│       RGA dma_buf→RGB 640×640    │   → GPU shader YUV→RGB │
│         → RKNN YOLOv5n            │   → 2×2 quad grid       │
│                                   │   → eglSwapBuffers     │
│                                   │   → Weston → HDMI       │
└───────────────────────────────────┴──────────────────────────┘
```

## 零拷贝链路

| 链路 | 实现 |
|------|------|
| 采集→编码 | `frame.fd` → MPP `mpp_buffer_import_fd` → 硬件编码 |
| 采集→显示 | `frame.fd` → `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, fd)` → `glEGLImageTargetTexture2DOES` → GPU shader |
| 采集→推理 | `frame.fd` → RGA `src.fd` → 缩放到 RGB 640×640 → RKNN |

## 模块与接口

### 1. frame_t — 通用帧描述符

```c
typedef struct {
    int      fd;        /* dma_buf fd (零拷贝核心) */
    void    *ptr;       /* mmap 指针, 仅 CPU 访问用 */
    uint32_t size;      /* 数据字节数 */
    uint32_t width, height, stride;
    uint32_t format;    /* V4L2_PIX_FMT_NV12 */
    int64_t  pts;
    uint8_t  cam_idx;   /* 0..3 */
    uint32_t seq;       /* 帧序号 */
} frame_t;
```

### 2. capture — V4L2 采集

```c
capture_t *cap_open(const char *dev, int w, int h, int fps, uint32_t fmt);
int  cap_dequeue(capture_t *c, frame_t *f);   /* blocking, 填充 f->fd, f->ptr, f->pts */
void cap_queue(capture_t *c);                  /* 归还 buffer */
void cap_close(capture_t *c);
```

V4L2_MEMORY_MMAP + VIDIOC_EXPBUF 导出 dma_buf fd。

### 3. display — Wayland + EGL + GLES2 渲染

```c
display_t *disp_open(int w, int h, int n_cams);
void disp_update(int cam_idx, const frame_t *f);  /* 存 fd, 渲染线程用 */
void disp_draw(display_t *d);     /* 4 quad 渲染 + eglSwapBuffers */
void disp_dispatch(display_t *d); /* Wayland 事件分发 */
void disp_close(display_t *d);
```

关键实现: `eglCreateImageKHR(dpy, ctx, EGL_LINUX_DMA_BUF_EXT, attrs, f->fd)` 导入 NV12 为 EGLImage, 创建 GL 纹理, GPU shader 做 YUV→RGB。

内部用 `EGL_EXT_image_dma_buf_import` 直接把 dma_buf fd 变成 GL 纹理, 不走 memcpy。着色器复用原厂 AVM 的 `texture_y_uv.frag`。

### 4. encoder — MPP 硬编码

```c
encoder_t *enc_open(int w, int h, int fps, int bitrate, const char *codec);
int  enc_feed(encoder_t *e, const frame_t *f, uint8_t **out, size_t *len);
void enc_close(encoder_t *e);
```

MPP import dma_buf fd, encode_put_frame + encode_get_packet。

### 5. pipeline — 多线程编排

```c
pipeline_t *pipe_new(int n_cams, capture_cfg *cam, encoder_cfg *enc, 
                     inference_cfg *inf, display_t *disp);
int  pipe_start(pipeline_t *p);
void pipe_stop(pipeline_t *p);
```

内部结构:
- 4 个 capture thread, 每个持有 capture_t + encoder_t
- 1 个 render thread, 持有 display_t
- ring buffer (depth=1, 覆盖旧帧) 连接 capture → render

### 6. inference — 可选 NPU 推理

```c
infer_t *infer_open(const char *model, int interval);
int  infer_detect(infer_t *inf, const frame_t *f, detection_t *out, int max);
void infer_close(infer_t *inf);
```

RGA 用 `src.fd = f->fd` 做零拷贝 NV12→RGB 640×640, 再喂 RKNN。

## 线程模型

```
main thread:
  ├─ pipe_start()
  ├─ while(!done) { disp_dispatch(); disp_draw(); usleep(33000); }
  └─ pipe_stop()

capture thread ×4 (pipe_start 创建):
  while(running) {
    cap_dequeue(cam, &f);
    ring_put(&display_rings[cam_idx], &f);   // 给显示
    enc_feed(enc, &f, &out, &len);          // 编码 (同步)
    if (inf && seq % N == 0)
      ring_put(&infer_ring, &f);            // 给推理 (可选)
    cap_queue(cam);
    f.seq++;
  }
```

- 编码在采集线程内同步: 避免额外线程切换, MPP 编码延迟 < 2ms 不阻塞采集
- 显示拿最新帧: ring depth=1, 渲染 30fps, 采集 25fps, 自动丢旧帧
- 推理异步: 独立线程, ring depth=1, 每 N 帧送一次

## 文件结构

```
src/
├── frame.h          # frame_t, ring_t 类型定义
├── capture.c/h      # V4L2 + dma_buf export
├── display.c/h      # Wayland + EGL + GLES2 + shader
├── encoder.c/h      # MPP h264/h265
├── pipeline.c/h     # 线程编排
├── inference.c/h    # RGA → RKNN (可选)
├── shader_yuv.h     # GPU shader NV12→RGB
├── xdg-shell-client.c/h  # Wayland 协议 (自动生成)
└── main.c           # 入口, CLI, 配置
```

## 成功标准

- 4路 1080p@25fps 采集 + H.265 编码, 不掉帧
- HDMI 2×2 画面实时显示, 延迟 < 100ms
- 全程 dma_buf 零拷贝, 无 memcpy 传帧
- 7 个源文件, 每个 < 300 行, 职责单一
