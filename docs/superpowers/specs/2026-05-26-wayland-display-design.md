# Wayland + EGL/GLES2 显示方案设计

## 目标

在 HDMI 屏幕上显示 4 路摄像头画面，2×2 布局。复用原厂 AVM 的 GPU shader YUV→RGB 方案，零拷贝、高帧率。

## 架构

```
Weston compositor (不关, 一直在跑)
  └── 我们的 Wayland 全屏客户端
        │
        ├── [采集线程 × 4] V4L2 DMA-BUF → NV12 帧
        │       └── capture.c (复用已有)
        │
        └── [渲染线程] Wayland + EGL + GLES2
                ├── NV12 → 2×GL_LUMINANCE 纹理 (Y + UV)
                ├── GPU shader YUV→RGB (抄原厂 texture_y_uv.frag)
                ├── 4 个 quad, 2×2 布局
                └── eglSwapBuffers 双缓冲
```

## 技术选型

| 层 | 技术 | 原因 |
|----|------|------|
| 窗口 | Wayland client | 运行在 Weston 之上, 不抢 DRM |
| GL上下文 | EGL on wl_egl_window | 标准 Wayland GL 方案 |
| 渲染 | OpenGL ES 2.0 | Mali G52 支持的 profile |
| YUV→RGB | GPU fragment shader | 零拷贝, 原厂方案 |
| 画面显示 | eglSwapBuffers | 交给 Wayland compositor 合成 |

## 着色器

直接从原厂 AVM 复制:
- `texture_y_uv.frag` — NV12 → RGB fragment shader
- `main_video_bias.frag` — 亮度/对比度调节 (可选)

## 不用的

- **RGA** — 显示不需要, GPU 做 YUV→RGB
- **Qt5** — 纯 C 实现, 不需要 Qt
- **DRM 直接操作** — 走 Wayland, 安全可靠

## 文件结构

```
src/
├── capture.c/h    (已有, V4L2采集)
├── encoder.c/h    (已有, MPP编码)
├── inference.c/h  (已有, NPU推理)
├── pipeline.c/h   (已有, 多线程管道)
├── main.c         (已有, 入口)
├── render.c/h     (新增, Wayland+EGL渲染)
├── shader_yuv.h   (新增, 内嵌着色器源码)
└── common.h       (已有, 共享类型)
```

## 帧率目标

> 25fps 显示, 4 路同时渲染
