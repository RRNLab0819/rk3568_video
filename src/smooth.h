/* src/smooth.h - lightweight EMA smoothing for OSD detection boxes
 *
 * Smoothing runs ONLY on the display side. Raw RKNN output is never modified.
 * Maintains per-class smoothed boxes and a persist counter so
 * boxes don't flicker or instantly vanish between inference cycles.
 *
 * Compiles as C (pipeline.c) or C++.
 */
#ifndef SMOOTH_H
#define SMOOTH_H

#include "frame.h"
#include <string.h>
#include <math.h>

#define SMOOTH_MAX_CLASSES 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    class_id;
    float  cx, cy, cw, ch;
    float  score;
    int    age;
} smooth_slot_t;

typedef struct {
    smooth_slot_t slots[SMOOTH_MAX_CLASSES];
    int           n_slots;
    float         alpha;
    int           max_persist;
} smooth_state_t;

/* Initialize smoother state */
static inline void smooth_init(smooth_state_t *s, float alpha, int max_persist)
{
    memset(s, 0, sizeof(*s));
    s->alpha = alpha;
    s->max_persist = (max_persist > 0) ? max_persist : 1;
}

/* Compute IoU between two axis-aligned boxes */
static inline float smooth_box_iou(float ax, float ay, float aw, float ah,
                                    float bx, float by, float bw, float bh)
{
    float x0 = fmaxf(ax, bx);
    float y0 = fmaxf(ay, by);
    float x1 = fminf(ax + aw, bx + bw);
    float y1 = fminf(ay + ah, by + bh);
    float iw = x1 - x0, ih = y1 - y0;
    if (iw <= 0.f || ih <= 0.f) return 0.f;
    float inter = iw * ih;
    float uni = aw * ah + bw * bh - inter;
    return (uni > 0.f) ? inter / uni : 0.f;
}

/* Feed new detections, produce smoothed output.
 * Returns number of smoothed detections written to dets_out.
 * dets_out may be the same pointer as dets (in-place).
 */
/* Only smooth cls=0 (person).  Non-person detections are silently dropped. */
static inline int smooth_update(smooth_state_t *s,
                                 const detection_t *dets, int n_in,
                                 detection_t *dets_out, int max_out)
{
    int matched[SMOOTH_MAX_CLASSES];
    int n_out = 0;
    memset(matched, 0, sizeof(matched));

    /* Only accept cls=0 (person) */
    for (int i = 0; i < n_in; i++) {
        if (dets[i].class_id != 0) continue;

        int best_j = -1;
        float best_iou = 0.35f;
        for (int j = 0; j < s->n_slots; j++) {
            if (matched[j]) continue;
            if (s->slots[j].class_id != 0) continue;
            float iou = smooth_box_iou(
                s->slots[j].cx - s->slots[j].cw * 0.5f,
                s->slots[j].cy - s->slots[j].ch * 0.5f,
                s->slots[j].cw, s->slots[j].ch,
                (float)dets[i].x, (float)dets[i].y,
                (float)dets[i].w, (float)dets[i].h);
            if (iou > best_iou) { best_iou = iou; best_j = j; }
        }

        if (best_j >= 0) {
            smooth_slot_t *sl = &s->slots[best_j];
            float a = s->alpha;
            float nc = (float)dets[i].x + (float)dets[i].w * 0.5f;
            float ny = (float)dets[i].y + (float)dets[i].h * 0.5f;
            sl->cx = a * sl->cx + (1.f - a) * nc;
            sl->cy = a * sl->cy + (1.f - a) * ny;
            sl->cw = a * sl->cw + (1.f - a) * (float)dets[i].w;
            sl->ch = a * sl->ch + (1.f - a) * (float)dets[i].h;
            sl->score = dets[i].confidence;
            sl->age = 0;
            matched[best_j] = 1;
        } else if (s->n_slots < SMOOTH_MAX_CLASSES) {
            smooth_slot_t *sl = &s->slots[s->n_slots++];
            sl->class_id = 0;
            sl->cx = (float)dets[i].x + (float)dets[i].w * 0.5f;
            sl->cy = (float)dets[i].y + (float)dets[i].h * 0.5f;
            sl->cw = (float)dets[i].w;
            sl->ch = (float)dets[i].h;
            sl->score = dets[i].confidence;
            sl->age = 0;
            matched[s->n_slots - 1] = 1;
        }
    }

    /* Age unmatched slots; emit those within persist window */
    for (int j = 0; j < s->n_slots; j++) {
        if (!matched[j]) s->slots[j].age++;
        if (s->slots[j].age <= s->max_persist && n_out < max_out) {
            smooth_slot_t *sl = &s->slots[j];
            dets_out[n_out].class_id   = 0;
            dets_out[n_out].confidence = sl->score;
            dets_out[n_out].x = (int)(sl->cx - sl->cw * 0.5f);
            dets_out[n_out].y = (int)(sl->cy - sl->ch * 0.5f);
            dets_out[n_out].w = (int)sl->cw;
            dets_out[n_out].h = (int)sl->ch;
            dets_out[n_out].cam_idx = dets[0].cam_idx;
            n_out++;
        }
    }

    /* Compact: remove fully aged-out slots */
    int write = 0;
    for (int j = 0; j < s->n_slots; j++) {
        if (s->slots[j].age <= s->max_persist)
            s->slots[write++] = s->slots[j];
    }
    s->n_slots = write;

    return n_out;
}

#ifdef __cplusplus
}
#endif

#endif /* SMOOTH_H */
