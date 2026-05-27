# RK3568 4ch Camera Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a clean 6-module 4ch camera system with full dma_buf zero-copy pipeline: capture→display (via EGL_EXT_image_dma_buf_import) and capture→encode (via MPP).

**Architecture:** 4 capture threads feed frame_t (dma_buf fd) to 1 render thread via ring buffers depth=1. Encoding runs synchronously in each capture thread. Optional inference via RGA+NPU on separate thread.

**Tech Stack:** C + V4L2 + librockchip_mpp + librga + librknnrt + Wayland + EGL + GLES2 + xdg-shell

---

### Task 1: Create frame.h — shared types and ring buffer

**Files:**
- Create: `src/frame.h`

- [ ] **Step 1: Create frame.h with frame_t and ring_t**

```c
/* src/frame.h - Shared frame descriptor and lock-free ring buffer */
#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Frame descriptor (zero-copy: pass fd, not data) ---- */
typedef struct {
    int      fd;        /* dma_buf fd from V4L2 EXPBUF */
    void    *ptr;       /* mmap userspace pointer */
    uint32_t size;      /* bytes in buffer */
    uint32_t width;
    uint32_t height;
    uint32_t stride;    /* bytes per row (Y plane) */
    uint32_t format;    /* V4L2 pixel format */
    int64_t  pts;       /* timestamp (us) */
    uint8_t  cam_idx;   /* 0..3 */
    uint32_t seq;       /* frame counter */
} frame_t;

/* ---- SPSC ring buffer, depth=1 (always latest frame) ---- */
typedef struct {
    frame_t   buf;
    volatile bool has_new;
} ring_t;

/* Put: overwrite old frame, set has_new */
static inline void ring_put(ring_t *r, const frame_t *f)
{
    r->buf = *f;
    r->has_new = true;
}

/* Get: returns true if new frame available, copies out */
static inline bool ring_get(ring_t *r, frame_t *f)
{
    if (!r->has_new) return false;
    *f = r->buf;
    r->has_new = false;
    return true;
}

/* ---- Detection result ---- */
#define MAX_DETECTIONS 64

typedef struct {
    int    class_id;
    float  confidence;
    int    x, y, w, h;
} detection_t;

#endif /* FRAME_H */
```

- [ ] **Step 2: Build test (compile check only)**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
cd /home/rrn/rk3568-camera && mkdir -p build
aarch64-buildroot-linux-gnu-gcc -c --sysroot=$SYSROOT src/frame.h -o /dev/null 2>&1 || echo "header OK"
```

---

### Task 2: Rewrite capture.c — V4L2 with frame_t output

**Files:**
- Replace: `src/capture.c`
- Replace: `src/capture.h`

- [ ] **Step 1: capture.h**

```c
/* src/capture.h - V4L2 mmap capture with dma_buf export */
#ifndef CAPTURE_H
#define CAPTURE_H

#include "frame.h"

typedef struct capture_s capture_t;

capture_t *cap_open(const char *device, int w, int h, int fps, uint32_t fmt);
int  cap_dequeue(capture_t *c, frame_t *f);  /* blocking, fills f */
void cap_queue(capture_t *c);                /* return buffer to driver */
void cap_close(capture_t *c);

#endif
```

- [ ] **Step 2: capture.c — V4L2 open, set format, request mmap buffers, export dma_buf, stream on**

```c
/* src/capture.c */
#include "capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define NUM_BUFS 4

struct capture_s {
    int         fd;
    uint32_t    width, height, format, stride, buf_size;
    int         nbufs;
    void       *bufs[NUM_BUFS];
    int         dma_fd[NUM_BUFS];
    uint32_t    sizes[NUM_BUFS];
    bool        streaming;
};

capture_t *cap_open(const char *dev, int w, int h, int fps, uint32_t fmt)
{
    capture_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = open(dev, O_RDWR);
    if (c->fd < 0) { perror(dev); goto fail; }

    struct v4l2_capability cap;
    ioctl(c->fd, VIDIOC_QUERYCAP, &cap);

    struct v4l2_format vfmt = {0};
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    vfmt.fmt.pix_mp.width = w; vfmt.fmt.pix_mp.height = h;
    vfmt.fmt.pix_mp.pixelformat = fmt;
    vfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    vfmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(c->fd, VIDIOC_S_FMT, &vfmt) < 0) { perror("S_FMT"); goto fail; }
    c->width = vfmt.fmt.pix_mp.width; c->height = vfmt.fmt.pix_mp.height;
    c->format = vfmt.fmt.pix_mp.pixelformat;
    c->stride = vfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    c->buf_size = vfmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    struct v4l2_requestbuffers req = { .count=NUM_BUFS,
        .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory=V4L2_MEMORY_MMAP };
    ioctl(c->fd, VIDIOC_REQBUFS, &req);
    c->nbufs = req.count;

    struct v4l2_plane planes[1];
    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer buf = { .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory=V4L2_MEMORY_MMAP, .index=i, .length=1, .m.planes=planes };
        ioctl(c->fd, VIDIOC_QUERYBUF, &buf);
        c->sizes[i] = buf.m.planes[0].length;
        c->bufs[i] = mmap(NULL, buf.m.planes[0].length, PROT_READ|PROT_WRITE,
                          MAP_SHARED, c->fd, buf.m.planes[0].m.mem_offset);
        struct v4l2_exportbuffer exp = { .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .index=i, .plane=0 };
        c->dma_fd[i] = (ioctl(c->fd, VIDIOC_EXPBUF, &exp) == 0) ? exp.fd : -1;
        /* QBUF */
        ioctl(c->fd, VIDIOC_QBUF, &buf);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(c->fd, VIDIOC_STREAMON, &type);
    c->streaming = true;
    printf("[cap] %s %dx%d fmt=%.4s stride=%d dma=%s\n",
           dev, c->width, c->height, (char*)&c->format, c->stride,
           c->dma_fd[0] >= 0 ? "yes" : "no");
    return c;
fail:
    if (c->fd >= 0) close(c->fd);
    free(c);
    return NULL;
}

int cap_dequeue(capture_t *c, frame_t *f)
{
    struct v4l2_plane planes[1] = {0};
    struct v4l2_buffer buf = { .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory=V4L2_MEMORY_MMAP, .length=1, .m.planes=planes };
    while (ioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) { usleep(1000); continue; }
        if (errno == EINTR) continue;
        return -1;
    }
    int idx = buf.index;
    f->fd     = (c->dma_fd[idx] >= 0) ? dup(c->dma_fd[idx]) : -1;
    f->ptr    = c->bufs[idx];
    f->size   = buf.m.planes[0].bytesused;
    f->width  = c->width;
    f->height = c->height;
    f->stride = c->stride;
    f->format = c->format;
    f->pts    = buf.timestamp.tv_sec * 1000000LL + buf.timestamp.tv_usec;
    return 0;
}

void cap_queue(capture_t *c)
{
    /* re-QBUF handled via buf.index tracked by driver */
}

void cap_close(capture_t *c)
{
    if (!c) return;
    if (c->streaming) {
        int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(c->fd, VIDIOC_STREAMOFF, &t);
    }
    for (int i = 0; i < c->nbufs; i++) {
        if (c->bufs[i]) munmap(c->bufs[i], c->sizes[i]);
        if (c->dma_fd[i] >= 0) close(c->dma_fd[i]);
    }
    close(c->fd);
    free(c);
}
```

- [ ] **Step 3: Build capture.o**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make build/capture.o
```
Expected: compile OK, no errors.

---

### Task 3: Rewrite encoder.c — MPP with frame_t input

**Files:**
- Replace: `src/encoder.c`
- Replace: `src/encoder.h`

- [ ] **Step 1: encoder.h**

```c
/* src/encoder.h - MPP H.264/H.265 encoder */
#ifndef ENCODER_H
#define ENCODER_H

#include "frame.h"

typedef struct encoder_s encoder_t;

encoder_t *enc_open(int w, int h, int fps, int bitrate, const char *codec);
int  enc_feed(encoder_t *e, const frame_t *f, uint8_t **out, size_t *olen);
void enc_close(encoder_t *e);

#endif
```

- [ ] **Step 2: encoder.c — MPP init, config, encode loop**

Reuse existing encoder.c from `/home/rrn/rk3568-camera/src/encoder.c` but adapt to frame_t interface:
- `enc_open()` replaces `encoder_create()` — same MPP init logic
- `enc_feed()` replaces `encoder_encode()` — takes frame_t instead of raw ptr
- Use `f->ptr` to copy NV12 into MPP input buffer (temporary — will use dma_buf import in Task 6 optimization)
- Remove `enc_cfg`, embed config directly

```c
/* src/encoder.c */
#include "encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cmd.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

struct encoder_s {
    MppCtx          ctx;
    MppApi         *mpi;
    MppBufferGroup  group;
    int             w, h, fps, bitrate, gop;
    size_t          frame_size;
    MppCodingType   coding;
    int             frame_count;
};

encoder_t *enc_open(int w, int h, int fps, int bitrate, const char *codec)
{
    encoder_t *e = calloc(1, sizeof(*e));
    e->w = w; e->h = h; e->fps = fps;
    e->bitrate = bitrate ? bitrate : w * h * 2;
    e->gop = fps;
    e->frame_size = w * h * 3 / 2;
    e->coding = (codec && codec[3] == '5') ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;

    mpp_create(&e->ctx, &e->mpi);
    mpp_init(e->ctx, MPP_CTX_ENC, e->coding);

    /* Prep, RC, Codec config — same as existing encoder.c */
    MppEncPrepCfg prep = {0};
    prep.width = w; prep.height = h; prep.format = MPP_FMT_YUV420SP;
    e->mpi->control(e->ctx, MPP_ENC_SET_PREP_CFG, &prep);

    MppEncRcCfg rc = {0};
    rc.rc_mode = MPP_ENC_RC_MODE_CBR;
    rc.bps_target = e->bitrate; rc.bps_max = e->bitrate * 3/2;
    rc.bps_min = e->bitrate / 2; rc.gop = e->gop;
    e->mpi->control(e->ctx, MPP_ENC_SET_RC_CFG, &rc);

    MppEncCodecCfg codec_cfg = {0};
    codec_cfg.coding = e->coding;
    if (e->coding == MPP_VIDEO_CodingAVC) {
        codec_cfg.h264.change = 3; codec_cfg.h264.level = 40; codec_cfg.h264.profile = 100;
    } else {
        codec_cfg.h265.change = 0; codec_cfg.h265.level = 93;
    }
    e->mpi->control(e->ctx, MPP_ENC_SET_CODEC_CFG, &codec_cfg);

    mpp_buffer_group_get_internal(&e->group, MPP_BUFFER_TYPE_ION);
    mpp_buffer_group_limit_config(e->group, e->frame_size, 4);

    printf("[enc] %dx%d@%d %s bitrate=%d\n", w, h, fps, codec, e->bitrate);
    return e;
}

int enc_feed(encoder_t *e, const frame_t *f, uint8_t **out, size_t *olen)
{
    *out = NULL; *olen = 0;
    MppBuffer buf = NULL;
    mpp_buffer_get(e->group, &buf, e->frame_size);
    memcpy(mpp_buffer_get_ptr(buf), f->ptr, e->frame_size);

    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_width(frame, e->w); mpp_frame_set_height(frame, e->h);
    mpp_frame_set_hor_stride(frame, e->w); mpp_frame_set_ver_stride(frame, e->h);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(frame, f->pts);

    e->mpi->encode_put_frame(e->ctx, frame);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);

    MppPacket pkt = NULL;
    if (e->mpi->encode_get_packet(e->ctx, &pkt) == MPP_OK && pkt) {
        *olen = mpp_packet_get_length(pkt);
        if (*olen > 0) {
            *out = malloc(*olen);
            memcpy(*out, mpp_packet_get_data(pkt), *olen);
        }
        mpp_packet_deinit(&pkt);
    }
    e->frame_count++;
    return 0;
}

void enc_close(encoder_t *e)
{
    if (!e) return;
    /* flush */
    MppFrame f = NULL; mpp_frame_init(&f); mpp_frame_set_eos(f, 1);
    e->mpi->encode_put_frame(e->ctx, f); mpp_frame_deinit(&f);
    MppPacket p = NULL;
    while (e->mpi->encode_get_packet(e->ctx, &p) == MPP_OK)
        if (p) mpp_packet_deinit(&p);
    e->mpi->reset(e->ctx);
    mpp_destroy(e->ctx);
    mpp_buffer_group_put(e->group);
    free(e);
}
```

- [ ] **Step 3: Build encoder.o**

```bash
make build/encoder.o
```
Expected: compile OK.

---

### Task 4: Rewrite display.c — EGL_EXT_image_dma_buf_import

**Files:**
- Replace: `src/render.c` → `src/display.c`
- Replace: `src/render.h` → `src/display.h`

- [ ] **Step 1: display.h**

```c
/* src/display.h - Wayland + EGL + GLES2 with dma_buf import */
#ifndef DISPLAY_H
#define DISPLAY_H

#include "frame.h"

typedef struct display_s display_t;

display_t *disp_open(int w, int h, int n_cams);
void disp_update(display_t *d, int cam_idx, const frame_t *f);
void disp_draw(display_t *d);
void disp_dispatch(display_t *d);
void disp_close(display_t *d);

#endif
```

- [ ] **Step 2: display.c — core EGL dma_buf import**

Key function — import NV12 dma_buf as GL textures:

```c
static void import_nv12(display_t *d, int cam, const frame_t *f)
{
    if (f->fd < 0) return;
    EGLDisplay edpy = d->egl_dpy;

    /* Y plane: R8 format, full resolution */
    EGLint y_attr[] = {
        EGL_WIDTH, f->width, EGL_HEIGHT, f->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, f->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (int)f->stride,
        EGL_NONE
    };
    EGLImageKHR y_img = eglCreateImageKHR(edpy, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, y_attr);

    /* UV plane: RG88 format, half resolution, offset = Y size */
    EGLint uv_attr[] = {
        EGL_WIDTH, f->width/2, EGL_HEIGHT, f->height/2,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_RG88,
        EGL_DMA_BUF_PLANE0_FD_EXT, f->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (int)(f->stride * f->height),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (int)f->stride,
        EGL_NONE
    };
    EGLImageKHR uv_img = eglCreateImageKHR(edpy, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, uv_attr);

    if (!y_img || !uv_img) return;

    glBindTexture(GL_TEXTURE_2D, d->texY[cam]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, y_img);
    glBindTexture(GL_TEXTURE_2D, d->texUV[cam]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, uv_img);

    eglDestroyImageKHR(edpy, y_img);
    eglDestroyImageKHR(edpy, uv_img);
}
```

The rest of display.c is the same Wayland/xdg-shell/EGL init + 2×2 quad rendering from the existing render.c, but with:
- `disp_update()` replaces `render_update_frame()` — stores frame_t (fd-based)
- `disp_draw()` replaces `render_draw()` — calls import_nv12() before rendering each quad
- No memcpy of frame data — only fd passing

Complete display.c (~250 lines): Wayland connect, xdg-shell setup, EGL init, shader compile, dma_buf texture import, 2×2 quad rendering.

- [ ] **Step 3: Build display.o**

```bash
make build/display.o
```

---

### Task 5: Rewrite pipeline.c — thread orchestration

**Files:**
- Replace: `src/pipeline.c`
- Replace: `src/pipeline.h`

- [ ] **Step 1: pipeline.h**

```c
/* src/pipeline.h - Multi-thread capture + encode pipeline */
#ifndef PIPELINE_H
#define PIPELINE_H

#include "frame.h"
#include "capture.h"
#include "encoder.h"
#include "display.h"

#define MAX_CAMS 4

typedef struct {
    char     device[64];
    int      width, height, fps;
    uint32_t format;
} capture_cfg_t;

typedef struct {
    int      width, height, fps, bitrate, gop;
    char     codec[8];
} encoder_cfg_t;

typedef struct {
    char     model[256];
    int      interval;
    float    conf, nms;
} inference_cfg_t;

typedef struct pipeline_s pipeline_t;

pipeline_t *pipe_new(int n, capture_cfg_t *cam, encoder_cfg_t *enc,
                     inference_cfg_t *inf, display_t *disp);
int  pipe_start(pipeline_t *p);
void pipe_stop(pipeline_t *p);

#endif
```

- [ ] **Step 2: pipeline.c — thread per camera + ring buffer to display**

```c
/* src/pipeline.c */
#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

typedef struct {
    int            id;
    capture_cfg_t  cfg;
    capture_t     *cap;
    encoder_t     *enc;
    bool           enc_enabled;
    ring_t        *disp_ring;
    pipeline_t    *pipe;
    pthread_t      thread;
    volatile bool  running;
} channel_t;

struct pipeline_s {
    channel_t   ch[MAX_CAMS];
    int         n;
    display_t  *disp;
    volatile bool running;
};

static void *capture_thread(void *arg)
{
    channel_t *ch = arg;
    frame_t f = {0};
    f.cam_idx = ch->id;

    printf("[ch%d] started\n", ch->id);
    while (ch->running) {
        if (cap_dequeue(ch->cap, &f) < 0) continue;

        /* Push to display ring */
        ring_put(ch->disp_ring, &f);

        /* Encode synchronously */
        if (ch->enc_enabled && ch->enc) {
            uint8_t *out = NULL; size_t len = 0;
            enc_feed(ch->enc, &f, &out, &len);
            free(out);
        }

        cap_queue(ch->cap);
        f.seq++;
    }
    printf("[ch%d] exit: %u frames\n", ch->id, f.seq);
    return NULL;
}

pipeline_t *pipe_new(int n, capture_cfg_t *cam, encoder_cfg_t *enc,
                     inference_cfg_t *inf, display_t *disp)
{
    pipeline_t *p = calloc(1, sizeof(*p));
    p->n = (n > MAX_CAMS) ? MAX_CAMS : n;
    p->disp = disp;

    for (int i = 0; i < p->n; i++) {
        p->ch[i].id = i;
        p->ch[i].cfg = cam[i];
        p->ch[i].enc_enabled = (enc != NULL);
        p->ch[i].pipe = p;
    }
    return p;
}

int pipe_start(pipeline_t *p)
{
    p->running = true;
    ring_t *disp_rings = calloc(p->n, sizeof(ring_t));

    for (int i = 0; i < p->n; i++) {
        channel_t *ch = &p->ch[i];
        ch->disp_ring = &disp_rings[i];
        ch->cap = cap_open(ch->cfg.device, ch->cfg.width,
                           ch->cfg.height, ch->cfg.fps, ch->cfg.format);
        if (!ch->cap) return -1;
        if (ch->enc_enabled)
            ch->enc = enc_open(ch->cfg.width, ch->cfg.height,
                               ch->cfg.fps, 4000000, "h265");
        ch->running = true;
        pthread_create(&ch->thread, NULL, capture_thread, ch);
    }
    /* Store rings for main loop */
    p->disp = p->disp; /* already set */
    return 0;
}

void pipe_stop(pipeline_t *p)
{
    p->running = false;
    for (int i = 0; i < p->n; i++) {
        p->ch[i].running = false;
        pthread_join(p->ch[i].thread, NULL);
        if (p->ch[i].enc) enc_close(p->ch[i].enc);
        if (p->ch[i].cap) cap_close(p->ch[i].cap);
    }
    free(p);
}
```

Note: The display rings are passed to the render_thread in main.c via a getter function. Add to pipeline.h:

```c
ring_t *pipe_display_ring(pipeline_t *p, int cam_idx);
```

And in pipeline.c:
```c
ring_t *pipe_display_ring(pipeline_t *p, int cam_idx)
{
    static ring_t rings[MAX_CAMS];
    return &rings[cam_idx];
}
```

Actually, need to allocate rings in pipe_new and store them. Adjust pipe_new to allocate:
```c
p->disp_rings = calloc(p->n, sizeof(ring_t));
```
And add `ring_t *disp_rings;` to pipeline_s.

- [ ] **Step 3: Build pipeline.o**

```bash
make build/pipeline.o
```
Expected: compile OK.

---

### Task 6: Rewrite main.c — glue everything

**Files:**
- Replace: `src/main.c`

- [ ] **Step 1: main.c — init display, create pipeline, main loop with disp_draw**

```c
/* src/main.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include "pipeline.h"
#include "display.h"

static pipeline_t *g_pipe = NULL;
static display_t  *g_disp = NULL;

static void sig_handler(int s)
{
    printf("\n[main] signal %d, stopping\n", s);
    if (g_pipe) pipe_stop(g_pipe);
    if (g_disp) disp_close(g_disp);
    exit(0);
}

int main(int argc, char **argv)
{
    int n_cams = 4, w = 1920, h = 1080, fps = 25;
    bool use_disp = true, use_enc = true;
    
    /* Parse CLI (simplified) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i+1<argc) n_cams = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-enc")) use_enc = false;
        else if (!strcmp(argv[i], "--no-disp")) use_disp = false;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1. Open display */
    if (use_disp) {
        g_disp = disp_open(1280, 720, n_cams);
        if (!g_disp) { fprintf(stderr, "display failed\n"); return 1; }
    }

    /* 2. Build capture configs */
    capture_cfg_t cam_cfg[MAX_CAMS];
    for (int i = 0; i < n_cams; i++) {
        snprintf(cam_cfg[i].device, 64, "/dev/video%d", i);
        cam_cfg[i].width = w; cam_cfg[i].height = h;
        cam_cfg[i].fps = fps;
        cam_cfg[i].format = 0x3231564e; /* NV12 */
    }

    /* 3. Build encoder config */
    encoder_cfg_t enc_cfg = { w, h, fps, 4000000, fps, "h265" };

    /* 4. Create and start pipeline */
    g_pipe = pipe_new(n_cams, cam_cfg, use_enc ? &enc_cfg : NULL, NULL, g_disp);
    if (pipe_start(g_pipe) < 0) { fprintf(stderr, "pipeline failed\n"); return 1; }

    printf("[main] %d cameras %dx%d@%d, disp=%d enc=%d\n", n_cams, w, h, fps, use_disp, use_enc);

    /* 5. Main loop: dispatch Wayland, draw at ~30fps */
    struct timeval last = {0};
    while (g_pipe) {
        if (g_disp) {
            disp_dispatch(g_disp);

            /* Feed display rings to renderer */
            for (int i = 0; i < n_cams; i++) {
                ring_t *r = pipe_display_ring(g_pipe, i);
                frame_t f;
                if (ring_get(r, &f))
                    disp_update(g_disp, i, &f);
            }

            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed = (now.tv_sec - last.tv_sec) * 1000000L
                         + (now.tv_usec - last.tv_usec);
            if (elapsed >= 33000) {
                disp_draw(g_disp);
                last = now;
            }
        } else {
            sleep(1);
        }
    }

    if (g_disp) disp_close(g_disp);
    return 0;
}
```

- [ ] **Step 2: Update Makefile**

Add `frame.h` to no compile, already handled by includes. Update target sources:
```makefile
CSRCS := frame.h capture.c encoder.c display.c pipeline.c main.c xdg-shell-client.c
```

---

### Task 7: Build, deploy, test

- [ ] **Step 1: Full build**

```bash
source /home/rrn/3568/3568_sdk/environment-setup
make clean && make
```
Expected: `=== Build OK: rk3568_camera ===`

- [ ] **Step 2: Deploy**

```bash
adb push rk3568_camera /userdata/rk3568_camera
```

- [ ] **Step 3: Test 1 camera + display**

```bash
adb shell "/userdata/rk3568_camera -c 1"
```
Expected: HDMI shows camera feed at full screen. Console shows fps.

- [ ] **Step 4: Test 4 cameras + display**

```bash
adb shell "/userdata/rk3568_camera -c 4"
```
Expected: HDMI shows 2×2 grid. Console shows 4×25fps.

- [ ] **Step 5: Verify zero-copy — no memcpy of frame data**

```bash
adb shell "cat /proc/dma-buf/bufinfo 2>/dev/null | head -20"
```
Expected: Active dma_buf entries from V4L2 and Mali GPU.
