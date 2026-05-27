/*
 * tools/rga_dmabuf_test.c - Isolated RGA dma_buf NV12->NV12 resize test
 *
 * Uses wrapbuffer_fd_t (dma_buf fd handle) for the SOURCE — matching the
 * factory AVM's actual RGA usage pattern. Destination uses virtualaddr for
 * CPU read-back verification.
 *
 * Build: make -f Makefile.rga_dmabuf
 * Run:   cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rga_dmabuf_test [device]
 *        default device: /dev/video0
 * Output: /tmp/rga_test_in_y.raw  (original frame, Y plane only for quick check)
 *         /tmp/rga_test_out.nv12   (resized output, 320x320 NV12)
 *         /tmp/rga_test_out_y.raw (resized output, Y plane only)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include <rga/im2d.h>
#include <rga/rga.h>

#define TARGET_W  320
#define TARGET_H  320
#define NUM_BUFS  4

/* ---- V4L2 NV12 capture (single frame) ---- */

static int xioctl(int fd, int req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static int capture_one_frame(const char *dev, int w, int h,
                             uint8_t **out_ptr, int *out_size,
                             int *out_stride, int *out_dma_fd)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return -1; }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); close(fd); return -1; }

    int stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    int buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("[capture] %s %dx%d stride=%d size=%d\n", dev,
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, stride, buf_size);

    struct v4l2_requestbuffers req = { .count = NUM_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); close(fd); return -1; }

    void *bufs[NUM_BUFS];
    int sizes[NUM_BUFS];
    int dma_fd[NUM_BUFS];
    struct v4l2_plane planes[1];

    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = planes };
        xioctl(fd, VIDIOC_QUERYBUF, &buf);
        sizes[i] = buf.m.planes[0].length;
        bufs[i] = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, buf.m.planes[0].m.mem_offset);
        struct v4l2_exportbuffer exp = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .index = i, .plane = 0 };
        dma_fd[i] = (xioctl(fd, VIDIOC_EXPBUF, &exp) == 0) ? exp.fd : -1;
        planes[0].length = sizes[i];
        planes[0].bytesused = 0;
        xioctl(fd, VIDIOC_QBUF, &buf);
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMON, &type);

    /* Dequeue one frame */
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = planes };
    while (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) { usleep(1000); continue; }
        perror("DQBUF"); break;
    }
    int idx = buf.index;

    printf("[capture] frame: index=%d bytesused=%d dma_fd=%d\n",
           idx, planes[0].bytesused, dma_fd[idx]);

    /* Return data to caller — caller must free *out_ptr */
    *out_ptr = malloc(buf_size);
    memcpy(*out_ptr, bufs[idx], buf_size);
    *out_size = buf_size;
    *out_stride = stride;
    *out_dma_fd = dup(dma_fd[idx]);  /* dup so caller owns it */

    /* QBUF and streamoff */
    planes[0].length = sizes[idx];
    planes[0].bytesused = 0;
    xioctl(fd, VIDIOC_QBUF, &buf);
    xioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < req.count; i++) {
        munmap(bufs[i], sizes[i]);
        if (dma_fd[i] >= 0) close(dma_fd[i]);
    }
    close(fd);
    return 0;
}

/* ---- RGA NV12->NV12 resize ---- */

static int rga_resize_nv12(int src_dma_fd, int src_w, int src_h, int src_stride,
                            uint8_t *dst_ptr, int dst_w, int dst_h)
{
    printf("[rga] src: fd=%d %dx%d stride=%d\n", src_dma_fd, src_w, src_h, src_stride);
    printf("[rga] dst: va=%p %dx%d\n", (void*)dst_ptr, dst_w, dst_h);

    /* Source: import dma_buf fd then wrap handle — factory-consistent path */
    im_handle_param_t hparam;
    memset(&hparam, 0, sizeof(hparam));
    hparam.width  = src_w;
    hparam.height = src_h;
    hparam.format = RK_FORMAT_YCbCr_420_SP;

    rga_buffer_handle_t src_handle = importbuffer_fd(src_dma_fd, &hparam);
    if (src_handle == 0) {
        fprintf(stderr, "[rga] importbuffer_fd for src failed: fd=%d %dx%d (handle=0)\n",
                src_dma_fd, src_w, src_h);
        return -1;
    }
    printf("[rga] importbuffer_fd: handle=0x%lx\n", (unsigned long)src_handle);

    rga_buffer_t src = wrapbuffer_handle(src_handle, src_w, src_h,
                                          RK_FORMAT_YCbCr_420_SP,
                                          src_stride, src_h);
    if (src.handle == 0) {
        fprintf(stderr, "[rga] wrapbuffer_handle for src failed (handle=0)\n");
        releasebuffer_handle(src_handle);
        return -1;
    }
    printf("[rga] src rga_buffer handle=0x%lx\n", (unsigned long)src.handle);

    /* Destination: import virtual address via im_handle_param */
    im_handle_param_t dst_param;
    memset(&dst_param, 0, sizeof(dst_param));
    dst_param.width  = dst_w;
    dst_param.height = dst_h;
    dst_param.format = RK_FORMAT_YCbCr_420_SP;

    rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(dst_ptr, &dst_param);
    if (dst_handle == 0) {
        fprintf(stderr, "[rga] importbuffer_virtualaddr for dst failed (handle=0)\n");
        releasebuffer_handle(src_handle);
        return -1;
    }
    printf("[rga] importbuffer_virtualaddr: handle=0x%lx\n", (unsigned long)dst_handle);

    rga_buffer_t dst = wrapbuffer_handle(dst_handle, dst_w, dst_h,
                                          RK_FORMAT_YCbCr_420_SP,
                                          dst_w, dst_h);
    if (dst.handle == 0) {
        fprintf(stderr, "[rga] wrapbuffer_handle for dst failed (handle=0)\n");
        releasebuffer_handle(dst_handle);
        releasebuffer_handle(src_handle);
        return -1;
    }
    printf("[rga] dst rga_buffer handle=0x%lx\n", (unsigned long)dst.handle);

    /* Fill dst with gray (Y=128, UV=128) as letterbox background */
    memset(dst_ptr, 128, dst_w * dst_h);
    memset(dst_ptr + dst_w * dst_h, 128, dst_w * dst_h / 2);

    /* Compute letterbox rect: fit src into dst, centered, maintain aspect */
    float src_ratio = (float)src_w / src_h;
    float dst_ratio = (float)dst_w / dst_h;
    int rw, rh, x_off, y_off;
    if (src_ratio > dst_ratio) {
        rw = dst_w;
        rh = (int)(dst_w / src_ratio);
        rh = rh & ~1;
    } else {
        rh = dst_h;
        rw = (int)(dst_h * src_ratio);
        rw = rw & ~3;
    }
    x_off = (dst_w - rw) / 2;
    y_off = (dst_h - rh) / 2;
    x_off = x_off & ~1;
    y_off = y_off & ~1;
    printf("[rga] letterbox: src %dx%d -> dst rect %dx%d offset (%d,%d)\n",
           src_w, src_h, rw, rh, x_off, y_off);

    im_rect srect = {0, 0, src_w, src_h};
    im_rect drect = {x_off, y_off, rw, rh};
    im_rect prect = {0, 0, 0, 0};
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));

    IM_STATUS ret = improcess(src, dst, pat, srect, drect, prect, 0);
    printf("[rga] improcess returned: %d (%s)\n", ret,
           ret == IM_STATUS_SUCCESS ? "SUCCESS" : "FAIL");

    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    if (src_dma_fd >= 0) close(src_dma_fd);
    return 0;
}

/* ---- Dump helpers ---- */

static void dump_nv12_y(const char *path, const uint8_t *nv12, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    for (int r = 0; r < h; r++)
        fwrite(nv12 + r * stride, 1, w, f);
    fclose(f);
    printf("[dump] wrote %s (%dx%d Y plane, stride=%d)\n", path, w, h, stride);
}

/* ---- Main ---- */

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/video0";
    int cam_w = 1920, cam_h = 1080;

    printf("=== RGA dma_buf NV12->NV12 resize test ===\n");
    printf("Device: %s, Source: %dx%d, Target: %dx%d\n\n",
           dev, cam_w, cam_h, TARGET_W, TARGET_H);

    /* Step 1: Capture one frame from V4L2, get dma_buf fd */
    uint8_t *src_ptr = NULL;
    int src_size = 0, src_stride = 0, src_dma_fd = -1;

    if (capture_one_frame(dev, cam_w, cam_h,
                          &src_ptr, &src_size, &src_stride, &src_dma_fd) < 0) {
        fprintf(stderr, "FAIL: V4L2 capture failed\n");
        return 1;
    }
    if (src_dma_fd < 0) {
        fprintf(stderr, "FAIL: no dma_buf fd exported\n");
        free(src_ptr);
        return 1;
    }

    /* Save source Y plane for comparison */
    dump_nv12_y("/tmp/rga_test_in_y.raw", src_ptr, cam_w, cam_h, src_stride);
    free(src_ptr);

    /* Step 2: Allocate destination buffer */
    int dst_size = TARGET_W * TARGET_H * 3 / 2;
    uint8_t *dst_ptr = (uint8_t *)malloc(dst_size);
    if (!dst_ptr) { close(src_dma_fd); return 1; }

    /* Step 3: RGA resize NV12->NV12 via dma_buf source */
    int r = rga_resize_nv12(src_dma_fd, cam_w, cam_h, src_stride,
                            dst_ptr, TARGET_W, TARGET_H);

    if (r < 0) {
        fprintf(stderr, "FAIL: RGA improcess failed\n");
        free(dst_ptr);
        return 1;
    }

    /* Step 4: Dump full output NV12 */
    {
        FILE *f = fopen("/tmp/rga_test_out.nv12", "wb");
        if (f) { fwrite(dst_ptr, 1, dst_size, f); fclose(f); }
        printf("[dump] wrote /tmp/rga_test_out.nv12 (%d bytes)\n", dst_size);
    }
    dump_nv12_y("/tmp/rga_test_out_y.raw", dst_ptr, TARGET_W, TARGET_H, TARGET_W);

    /* Step 5: Quick sanity — check output is not all-gray */
    int nonzero = 0;
    for (int i = 0; i < TARGET_W * TARGET_H; i++)
        if (dst_ptr[i] != 128) nonzero++;
    printf("[check] non-gray Y pixels: %d / %d (%.1f%%)\n",
           nonzero, TARGET_W * TARGET_H,
           100.0f * nonzero / (TARGET_W * TARGET_H));

    if (nonzero < 100)
        fprintf(stderr, "WARNING: output appears mostly gray. Check camera input.\n");

    free(dst_ptr);

    printf("\n=== Test complete ===\n");
    printf("Verify: scp /tmp/rga_test_in_y.raw and /tmp/rga_test_out_y.raw\n");
    printf("  ffplay -f rawvideo -pix_fmt gray -video_size %dx%d /tmp/rga_test_in_y.raw\n",
           cam_w, cam_h);
    printf("  ffplay -f rawvideo -pix_fmt gray -video_size %dx%d /tmp/rga_test_out_y.raw\n",
           TARGET_W, TARGET_H);
    return 0;
}
