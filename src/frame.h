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
    int    cam_idx;      /* which camera produced this detection */
} detection_t;

#endif /* FRAME_H */
