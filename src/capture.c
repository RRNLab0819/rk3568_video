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
    int         last_idx;   /* tracked for cap_queue */
    bool        streaming;
};

capture_t *cap_open(const char *dev, int w, int h, int fps, uint32_t fmt)
{
    (void)fps;
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

    /* Try to increase camera brightness/exposure for low-light scenes */
    {
        struct v4l2_queryctrl qc = {.id = V4L2_CID_EXPOSURE_AUTO};
        if (ioctl(c->fd, VIDIOC_QUERYCTRL, &qc) == 0) {
            struct v4l2_control ctl = {.id = V4L2_CID_EXPOSURE_AUTO, .value = V4L2_EXPOSURE_MANUAL};
            ioctl(c->fd, VIDIOC_S_CTRL, &ctl);
        }
    }
    {
        struct v4l2_queryctrl qc = {.id = V4L2_CID_EXPOSURE};
        if (ioctl(c->fd, VIDIOC_QUERYCTRL, &qc) == 0) {
            /* Push exposure higher — use 75% of max */
            int val = qc.minimum + (qc.maximum - qc.minimum) * 3 / 4;
            if (val > qc.maximum) val = qc.maximum;
            struct v4l2_control ctl = {.id = V4L2_CID_EXPOSURE, .value = val};
            if (ioctl(c->fd, VIDIOC_S_CTRL, &ctl) == 0)
                printf("[cap] exposure set to %d (range %d-%d)\n", val, qc.minimum, qc.maximum);
        }
    }
    {
        struct v4l2_queryctrl qc = {.id = V4L2_CID_GAIN};
        if (ioctl(c->fd, VIDIOC_QUERYCTRL, &qc) == 0) {
            int val = qc.minimum + (qc.maximum - qc.minimum) * 3 / 4;
            if (val > qc.maximum) val = qc.maximum;
            struct v4l2_control ctl = {.id = V4L2_CID_GAIN, .value = val};
            if (ioctl(c->fd, VIDIOC_S_CTRL, &ctl) == 0)
                printf("[cap] gain set to %d (range %d-%d)\n", val, qc.minimum, qc.maximum);
        }
    }

    struct v4l2_requestbuffers req = { .count=NUM_BUFS,
        .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory=V4L2_MEMORY_MMAP };
    if (ioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); goto fail; }
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
        planes[0].length = c->sizes[i];
        planes[0].bytesused = 0;
        ioctl(c->fd, VIDIOC_QBUF, &buf);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); goto fail; }
    c->streaming = true;
    printf("[cap] %s %dx%d fmt=%.4s stride=%d dma_fd=%d\n",
           dev, c->width, c->height, (char*)&c->format, c->stride,
           c->dma_fd[0] >= 0 ? c->dma_fd[0] : -1);
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
    c->last_idx = idx;
    f->fd     = (c->dma_fd[idx] >= 0) ? dup(c->dma_fd[idx]) : -1;
    f->ptr    = c->bufs[idx];
    f->size   = buf.m.planes[0].bytesused;
    f->width  = c->width;
    f->height = c->height;
    f->stride = c->stride;
    f->format = c->format;
    f->pts    = (int64_t)buf.timestamp.tv_sec * 1000000LL + buf.timestamp.tv_usec;
    return 0;
}

void cap_queue(capture_t *c)
{
    if (c->last_idx < 0 || c->last_idx >= c->nbufs) return;
    struct v4l2_plane planes[1] = {0};
    planes[0].length = c->sizes[c->last_idx];
    planes[0].bytesused = 0;
    struct v4l2_buffer buf = { .type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory=V4L2_MEMORY_MMAP, .index=c->last_idx, .length=1, .m.planes=planes };
    ioctl(c->fd, VIDIOC_QBUF, &buf);
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
