/* Capture a single raw NV12 frame from V4L2 device.
 * Minimal standalone tool — no Wayland, no EGL, no MPP needed.
 * Build: gcc -O2 -o grab_frame grab_frame.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define WIDTH  1920
#define HEIGHT 1080

int main(int argc, char **argv)
{
    const char *dev   = (argc > 1) ? argv[1] : "/dev/video0";
    const char *out   = (argc > 2) ? argv[2] : "/tmp/frame.nv12";
    int n_frames      = (argc > 3) ? atoi(argv[3]) : 1;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    /* Set format */
    struct v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width    = WIDTH;
    fmt.fmt.pix_mp.height   = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.num_planes   = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); close(fd); return 1; }
    printf("[grab] %s: %dx%d NV12 planes=%d size=%d\n",
           dev, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.num_planes, fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    /* Request buffers */
    struct v4l2_requestbuffers req = {0};
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); close(fd); return 1; }
    printf("[grab] %u buffers requested\n", req.count);

    /* Map buffers */
    struct v4l2_buffer buf[4];
    void *ptr[4];
    for (unsigned int i = 0; i < req.count; i++) {
        memset(&buf[i], 0, sizeof(buf[i]));
        buf[i].type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf[i].memory = V4L2_MEMORY_MMAP;
        buf[i].index  = i;
        struct v4l2_plane plane = {0};
        buf[i].m.planes = &plane;
        buf[i].length   = 1;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf[i]) < 0) { perror("VIDIOC_QUERYBUF"); close(fd); return 1; }
        ptr[i] = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
        if (ptr[i] == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    }

    /* Queue all buffers */
    for (unsigned int i = 0; i < req.count; i++) {
        if (ioctl(fd, VIDIOC_QBUF, &buf[i]) < 0) { perror("VIDIOC_QBUF"); close(fd); return 1; }
    }

    /* Start streaming */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); close(fd); return 1; }
    printf("[grab] streaming started, capturing %d frames...\n", n_frames);

    /* Capture frames */
    FILE *fp = fopen(out, "wb");
    if (!fp) { perror("fopen"); close(fd); return 1; }

    for (int n = 0; n < n_frames; n++) {
        struct v4l2_buffer b = {0};
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        struct v4l2_plane plane = {0};
        b.m.planes = &plane;
        b.length   = 1;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("VIDIOC_DQBUF"); break; }

        unsigned int sz = plane.bytesused;
        printf("[grab] frame %d: %u bytes\n", n, sz);
        fwrite(ptr[b.index], 1, sz, fp);

        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); break; }
    }

    fclose(fp);

    /* Stop streaming */
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    /* Unmap */
    for (unsigned int i = 0; i < req.count; i++)
        munmap(ptr[i], buf[i].m.planes[0].length);

    close(fd);
    printf("[grab] done → %s\n", out);
    return 0;
}
