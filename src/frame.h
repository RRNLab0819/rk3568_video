/* src/frame.h - Shared frame descriptor and lock-free ring buffer */
#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* ---- Frame descriptor (zero-copy: pass fd, not data) ---- */
typedef struct {
    int      fd;        /* dma_buf fd from V4L2 EXPBUF */
    void    *ptr;       /* mmap or malloc'd userspace pointer */
    bool     own_ptr;   /* true if ptr was malloc'd (deep copy) — must free */
    uint32_t size;      /* bytes in buffer */
    uint32_t width;
    uint32_t height;
    uint32_t stride;    /* bytes per row (Y plane) */
    uint32_t format;    /* V4L2 pixel format */
    int64_t  pts;       /* timestamp (us) */
    uint8_t  cam_idx;   /* 0..3 */
    uint32_t seq;       /* per-camera frame counter */
    uint32_t cap_seq;   /* global capture sequence (monotonic across all cams) */
} frame_t;

/* ---- SPSC ring buffer, depth=1 (always latest frame) ---- */
typedef struct {
    frame_t         buf;
    bool            has_new;
    pthread_mutex_t lock;
} ring_t;

static inline void frame_release(frame_t *f)
{
    if (!f) return;
    if (f->fd >= 0) close(f->fd);
    if (f->own_ptr && f->ptr) free(f->ptr);
    f->fd = -1;
    f->ptr = NULL;
    f->own_ptr = false;
}

static inline bool frame_clone_packed_nv12(frame_t *dst, const frame_t *src, bool keep_fd)
{
    if (!dst || !src || !src->ptr || src->width == 0 || src->height == 0) return false;

    uint32_t ysz = src->width * src->height;
    uint32_t uvsz = ysz / 2;
    uint8_t *mem = (uint8_t *)malloc(ysz + uvsz);
    if (!mem) return false;

    uint8_t *d = mem;
    const uint8_t *s = (const uint8_t *)src->ptr;
    for (uint32_t r = 0; r < src->height; r++) {
        memcpy(d, s, src->width);
        s += src->stride;
        d += src->width;
    }

    s = (const uint8_t *)src->ptr + src->stride * src->height;
    for (uint32_t r = 0; r < src->height / 2; r++) {
        memcpy(d, s, src->width);
        s += src->stride;
        d += src->width;
    }

    *dst = *src;
    dst->ptr = mem;
    dst->own_ptr = true;
    dst->stride = src->width;
    dst->size = ysz + uvsz;
    dst->fd = (keep_fd && src->fd >= 0) ? dup(src->fd) : -1;
    return true;
}

static inline void ring_init(ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->buf.fd = -1;
    pthread_mutex_init(&r->lock, NULL);
}

static inline void ring_clear(ring_t *r)
{
    if (!r) return;
    pthread_mutex_lock(&r->lock);
    if (r->has_new) {
        frame_release(&r->buf);
        r->has_new = false;
    }
    pthread_mutex_unlock(&r->lock);
}

/* Put: overwrite old frame if not yet consumed, set has_new */
static inline void ring_put(ring_t *r, const frame_t *f)
{
    pthread_mutex_lock(&r->lock);
    /* If consumer hasn't read the previous frame, clean it up to avoid fd/mem leak */
    if (r->has_new)
        frame_release(&r->buf);
    r->buf = *f;
    r->has_new = true;
    pthread_mutex_unlock(&r->lock);
}

/* Get: returns true if new frame available, copies out */
static inline bool ring_get(ring_t *r, frame_t *f)
{
    bool ok = false;
    pthread_mutex_lock(&r->lock);
    if (r->has_new) {
        *f = r->buf;
        r->has_new = false;
        ok = true;
    }
    pthread_mutex_unlock(&r->lock);
    return ok;
}

/* ---- Detection result ---- */
#define MAX_DETECTIONS 64

typedef struct {
    int    class_id;
    float  confidence;
    int    x, y, w, h;
    int    cam_idx;      /* which camera produced this detection */
} detection_t;

#endif /* FRAME_H */
