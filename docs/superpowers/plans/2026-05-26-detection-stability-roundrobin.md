# Detection Stability + Round-Robin + OSD Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add detection smoothing, OSD coordinate verification, 4-channel round-robin inference, performance stats, and config enhancements without breaking the verified YOLOv5 inference pipeline.

**Architecture:** Single RKNN context, single inference thread polling channels round-robin. Per-channel detection rings with EMA smoothing on OSD output only (raw RKNN output unchanged). Display maps original-image coordinates to tile coordinates per-channel. Stats printed every 5 seconds.

**Tech Stack:** C (capture/display/encoder), C++ (inference/postprocess), Wayland/EGL/GLES2 display, RKNN runtime

**Invariants (DO NOT BREAK):**
- inference.cc: UINT8 NHWC input, no XOR 0x80, no brightness, no auto-contrast
- postprocess.cc: official model zoo logic
- capture.c/display.c/encoder.c: zero-copy NV12 pipeline

---

### Task 1: Add cam_idx to detection_t and extend config

**Files:**
- Modify: `src/frame.h:47-51`
- Modify: `src/pipeline.h:21-25`
- Modify: `config.ini:17-22`

- [ ] **Step 1: Add cam_idx and label string to detection_t**

In `src/frame.h`, replace the detection_t struct:

```c
/* ---- Detection result ---- */
#define MAX_DETECTIONS 64

typedef struct {
    int    class_id;
    float  confidence;
    int    x, y, w, h;
    int    cam_idx;      /* which camera produced this detection */
} detection_t;
```

- [ ] **Step 2: Extend inference_cfg_t in pipeline.h**

Replace lines 21-25:

```c
typedef struct {
    char     model[256];
    int      interval;
    float    conf, nms;
    bool     smooth_enable;
    float    smooth_alpha;
    int      min_persist;
    char     channels[16];    /* e.g. "0,1,2,3" */
    bool     round_robin;
} inference_cfg_t;
```

- [ ] **Step 3: Update config.ini**

Replace the [inference] section:

```ini
[inference]
enabled        = false        # true to enable AI detection
model          = /userdata/yolov5.rknn
interval       = 5            # run inference every N frames
conf           = 0.20         # confidence threshold
nms            = 0.45         # NMS threshold
smooth_enable  = true         # OSD box smoothing
smooth_alpha   = 0.6          # EMA alpha (0=no smooth, 1=freeze)
min_persist    = 2            # hold last box for N cycles when lost
channels       = 0,1,2,3      # which cameras to run inference on
round_robin    = true         # true=poll all channels, false=cam0 only
```

- [ ] **Step 4: Build to verify no compile errors**

Run: `source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1 | tail -5`
Expected: `=== Build OK: rk3568_camera ===`

---

### Task 2: Detection smoothing helper (pipeline only, not inference)

**Files:**
- Create: `src/smooth.h`

- [ ] **Step 1: Create src/smooth.h**

```c
/* src/smooth.h - lightweight EMA smoothing for OSD detection boxes
 *
 * Smoothing runs ONLY on the display side. Raw RKNN output is never modified.
 * This module maintains per-class smoothed boxes and a persist counter so
 * boxes don't flicker or instantly vanish between inference cycles.
 */
#ifndef SMOOTH_H
#define SMOOTH_H

#include "frame.h"
#include <string.h>
#include <math.h>

#define SMOOTH_MAX_CLASSES 8  /* track up to 8 classes simultaneously */

typedef struct {
    int    class_id;
    float  cx, cy, cw, ch;    /* smoothed center + size */
    float  score;
    int    age;                /* frames since last update */
} smooth_slot_t;

typedef struct {
    smooth_slot_t slots[SMOOTH_MAX_CLASSES];
    int           n_slots;
    float         alpha;      /* 0=instant, 1=frozen */
    int           max_persist; /* max frames to hold stale box */
} smooth_state_t;

/* Initialize smoother */
static inline void smooth_init(smooth_state_t *s, float alpha, int max_persist)
{
    memset(s, 0, sizeof(*s));
    s->alpha = alpha;
    s->max_persist = (max_persist > 0) ? max_persist : 1;
}

/* Compute IoU between two axis-aligned boxes (for matching) */
static inline float box_iou(float ax, float ay, float aw, float ah,
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

/* Feed new detections: each new det matched by class+IoU to existing slot.
 * Unmatched slots age out. Matched slots get EMA update. */
static inline int smooth_update(smooth_state_t *s,
                                 const detection_t *dets, int n_in,
                                 detection_t *dets_out, int max_out)
{
    bool matched[SMOOTH_MAX_CLASSES] = {false};
    int  n_out = 0;

    /* Match each incoming detection to a slot (by class + IoU > 0.3) */
    for (int i = 0; i < n_in; i++) {
        int best_j = -1;
        float best_iou = 0.35f;  /* IoU threshold for matching */
        for (int j = 0; j < s->n_slots; j++) {
            if (matched[j]) continue;
            if (s->slots[j].class_id != dets[i].class_id) continue;
            float iou = box_iou(s->slots[j].cx - s->slots[j].cw * 0.5f,
                                s->slots[j].cy - s->slots[j].ch * 0.5f,
                                s->slots[j].cw, s->slots[j].ch,
                                (float)dets[i].x, (float)dets[i].y,
                                (float)dets[i].w, (float)dets[i].h);
            if (iou > best_iou) { best_iou = iou; best_j = j; }
        }
        if (best_j >= 0) {
            /* EMA update matched slot */
            smooth_slot_t *sl = &s->slots[best_j];
            float a = s->alpha;
            float nc = (float)dets[i].x + (float)dets[i].w * 0.5f;  /* center x */
            float ny = (float)dets[i].y + (float)dets[i].h * 0.5f;  /* center y */
            sl->cx = a * sl->cx + (1.f - a) * nc;
            sl->cy = a * sl->cy + (1.f - a) * ny;
            sl->cw = a * sl->cw + (1.f - a) * (float)dets[i].w;
            sl->ch = a * sl->ch + (1.f - a) * (float)dets[i].h;
            sl->score = dets[i].confidence;
            sl->age = 0;
            matched[best_j] = true;
        } else if (s->n_slots < SMOOTH_MAX_CLASSES) {
            /* New class tracked */
            smooth_slot_t *sl = &s->slots[s->n_slots++];
            sl->class_id = dets[i].class_id;
            sl->cx = (float)dets[i].x + (float)dets[i].w * 0.5f;
            sl->cy = (float)dets[i].y + (float)dets[i].h * 0.5f;
            sl->cw = (float)dets[i].w;
            sl->ch = (float)dets[i].h;
            sl->score = dets[i].confidence;
            sl->age = 0;
            matched[s->n_slots - 1] = true;
        }
    }

    /* Age unmatched slots; emit slots within persist window */
    for (int j = 0; j < s->n_slots; j++) {
        if (!matched[j]) s->slots[j].age++;
        if (s->slots[j].age <= s->max_persist && n_out < max_out) {
            smooth_slot_t *sl = &s->slots[j];
            dets_out[n_out].class_id  = sl->class_id;
            dets_out[n_out].confidence = sl->score;
            dets_out[n_out].x = (int)(sl->cx - sl->cw * 0.5f);
            dets_out[n_out].y = (int)(sl->cy - sl->ch * 0.5f);
            dets_out[n_out].w = (int)sl->cw;
            dets_out[n_out].h = (int)sl->ch;
            dets_out[n_out].cam_idx = dets[0].cam_idx;  /* propagate cam_idx */
            n_out++;
        }
    }

    /* Compact: remove slots that have aged out completely */
    int write = 0;
    for (int j = 0; j < s->n_slots; j++) {
        if (s->slots[j].age <= s->max_persist)
            s->slots[write++] = s->slots[j];
    }
    s->n_slots = write;

    return n_out;
}

#endif /* SMOOTH_H */
```

- [ ] **Step 2: Build to verify**

Run: `source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1 | tail -5`
Expected: `=== Build OK: rk3568_camera ===`

---

### Task 3: Per-channel inference rings and round-robin pipeline

**Files:**
- Modify: `src/pipeline.h:37` (pipe_get_detections signature)
- Modify: `src/pipeline.c` (infer_thread rewrite, per-channel rings, stats)

- [ ] **Step 1: Read full current pipeline.c for reference**

Already in context. Key structs to modify:
- `channel_t`: add `infer_ring` to all channels (currently only ch0 has it)
- `pipeline_s`: add per-channel detection storage, smooth states, stats

- [ ] **Step 2: Modify pipeline_s struct in pipeline.c (lines 25-38)**

Replace lines 25-38:

```c
typedef struct {
    int            id;
    capture_cfg_t  cfg;
    capture_t     *cap;
    encoder_t     *enc;
    ring_t        *disp_ring;
    ring_t        *enc_ring;
    ring_t        *infer_ring;  /* NULL if this channel not in inference */
    pipeline_t    *pipe;
    pthread_t      cap_thr, enc_thr;
    volatile bool  running;
    int            frame_count;
} channel_t;

struct pipeline_s {
    channel_t         ch[MAX_CAMS];
    ring_t            disp_rings[MAX_CAMS];
    ring_t            enc_rings[MAX_CAMS];
    ring_t            infer_rings[MAX_CAMS];  /* one ring per channel */
    encoder_cfg_t    *enc_cfg;
    inference_cfg_t  *inf_cfg;
    detection_t       dets[MAX_CAMS][MAX_DETECTIONS];  /* per-channel */
    int               det_count[MAX_CAMS];
    smooth_state_t    smooth[MAX_CAMS];  /* per-channel smoothing */
    pthread_mutex_t   det_lock;
    pthread_t         infer_thr;
    int               n, max_frames;
    volatile bool     running;

    /* Perf stats */
    struct {
        int      cap_frames[MAX_CAMS];
        int      enc_frames[MAX_CAMS];
        int      inf_total;
        int      inf_per_ch[MAX_CAMS];
        float    inf_latency_ms;
        int      disp_frames;
        int      dropped_rings[MAX_CAMS];
        time_t   last_report;
    } stats;
};
```

- [ ] **Step 3: Add infer_ring to all channels in pipe_new (around line 174)**

Replace the infer_ring assignment block:

```c
    for (int i = 0; i < p->n; i++) {
        p->ch[i].id = i; p->ch[i].cfg = cam[i]; p->ch[i].pipe = p;
        p->ch[i].disp_ring = &p->disp_rings[i];
        if (enc) p->ch[i].enc_ring = &p->enc_rings[i];
        p->ch[i].infer_ring = &p->infer_rings[i];  /* all channels have ring */
    }
```

- [ ] **Step 4: Modify capture_thread to feed per-channel infer ring (around line 55-72)**

Replace the infer_ring push section:

```c
        if (ch->infer_ring && ch->pipe->inf_cfg &&
            (ch->frame_count % ch->pipe->inf_cfg->interval == 0)) {
            /* Deep copy NV12 */
            frame_t fcopy = f;
            int ysz = f.width * f.height, uvsz = ysz / 2;
            fcopy.ptr = malloc(ysz + uvsz);
            if (fcopy.ptr) {
                uint8_t *d = fcopy.ptr, *s = f.ptr;
                for (int r = 0; r < f.height; r++)
                    { memcpy(d, s, f.width); s += f.stride; d += f.width; }
                s = (uint8_t*)f.ptr + f.stride * f.height;
                for (int r = 0; r < f.height/2; r++)
                    { memcpy(d, s, f.width); s += f.stride; d += f.width; }
                ring_put(ch->infer_ring, &fcopy);  /* deep copy for inference */
            }
        }
```

Same logic, just now all channels push to their own ring. The original code only did it for ch0 because only ch0 had infer_ring. Now all channels have it.

- [ ] **Step 5: Rewrite infer_thread for round-robin (around line 118-158)**

Replace the entire infer_thread function:

```c
static void *infer_thread(void *arg)
{
    pipeline_t *p = arg;
    infer_t *inf = infer_open(p->inf_cfg->model, p->inf_cfg->conf, p->inf_cfg->nms);
    if (!inf) { fprintf(stderr, "[infer] model load failed\n"); return NULL; }

    /* Init per-channel smooth state */
    for (int i = 0; i < MAX_CAMS; i++) {
        smooth_init(&p->smooth[i],
                    p->inf_cfg->smooth_alpha,
                    p->inf_cfg->min_persist);
    }

    /* Parse channel list: "0,1,2,3" -> int array */
    int ch_list[MAX_CAMS] = {0};
    int n_ch = 0;
    {
        const char *s = p->inf_cfg->channels;
        while (*s && n_ch < MAX_CAMS) {
            while (*s == ' ' || *s == ',') s++;
            if (*s >= '0' && *s <= '9') {
                ch_list[n_ch++] = atoi(s);
                while (*s >= '0' && *s <= '9') s++;
            }
        }
    }
    if (n_ch == 0) { ch_list[0] = 0; n_ch = 1; }  /* default: cam0 only */

    int current_ch = 0;
    struct timeval t_start, t_end;

    while (p->running) {
        int cam = ch_list[current_ch];
        frame_t f;
        if (!ring_get(&p->infer_rings[cam], &f)) {
            /* No frame for this channel, try next */
            current_ch = (current_ch + 1) % n_ch;
            usleep(1000);
            continue;
        }

        /* Run inference */
        gettimeofday(&t_start, NULL);
        detection_t raw_dets[MAX_DETECTIONS];
        int n_raw = infer_detect(inf, &f, raw_dets, MAX_DETECTIONS);
        gettimeofday(&t_end, NULL);

        /* Tag detections with cam_idx */
        for (int i = 0; i < n_raw; i++)
            raw_dets[i].cam_idx = cam;

        float lat_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0f +
                       (t_end.tv_usec - t_start.tv_usec) / 1000.0f;

        /* Smooth + persist for OSD */
        detection_t smooth_dets[MAX_DETECTIONS];
        int n_smooth = n_raw;
        if (p->inf_cfg->smooth_enable && n_raw >= 0) {
            n_smooth = smooth_update(&p->smooth[cam], raw_dets, n_raw,
                                      smooth_dets, MAX_DETECTIONS);
        } else {
            memcpy(smooth_dets, raw_dets, n_raw * sizeof(detection_t));
        }

        /* Update per-channel detections (thread-safe) */
        pthread_mutex_lock(&p->det_lock);
        p->det_count[cam] = n_smooth;
        if (n_smooth > 0)
            memcpy(p->dets[cam], smooth_dets, n_smooth * sizeof(detection_t));
        pthread_mutex_unlock(&p->det_lock);

        /* Update stats */
        p->stats.inf_total++;
        p->stats.inf_per_ch[cam]++;
        p->stats.inf_latency_ms = lat_ms;

        if (n_raw > 0) {
            fprintf(stderr, "[infer] cam%d: %d raw dets, %.1f ms "
                    "top: cls=%d conf=%.2f\n",
                    cam, n_raw, lat_ms,
                    raw_dets[0].class_id, raw_dets[0].confidence);
        }

        /* Advance round-robin */
        current_ch = (current_ch + 1) % n_ch;

        /* Cleanup deep-copied frame */
        if (f.fd >= 0) close(f.fd);
        free(f.ptr);
    }

    /* Drain remaining frames */
    for (int i = 0; i < n_ch; i++) {
        int cam = ch_list[i];
        frame_t f;
        while (ring_get(&p->infer_rings[cam], &f)) {
            if (f.fd >= 0) close(f.fd);
            free(f.ptr);
        }
    }

    infer_close(inf);
    printf("[infer] exit: %d inferences total\n", p->stats.inf_total);
    return NULL;
}
```

- [ ] **Step 6: Update pipe_get_detections to return per-channel (pipeline.c line 248-257, pipeline.h line 37)**

In `pipeline.h`, change the function signature:

```c
int  pipe_get_detections(pipeline_t *p, detection_t *dets, int max_dets);
/* -> returns total detections across all channels */
```

In `pipeline.c`, replace pipe_get_detections:

```c
int pipe_get_detections(pipeline_t *p, detection_t *dets, int max_dets)
{
    if (!p || !dets) return 0;
    pthread_mutex_lock(&p->det_lock);
    int n = 0;
    for (int c = 0; c < p->n && n < max_dets; c++) {
        int nc = p->det_count[c];
        if (nc > max_dets - n) nc = max_dets - n;
        if (nc > 0) {
            memcpy(dets + n, p->dets[c], nc * sizeof(detection_t));
            n += nc;
        }
    }
    pthread_mutex_unlock(&p->det_lock);
    return n;
}
```

- [ ] **Step 7: Add perf stats function**

Add to pipeline.c, before pipe_get_detections:

```c
void pipe_print_stats(pipeline_t *p)
{
    if (!p) return;
    time_t now = time(NULL);
    if (p->stats.last_report == 0) p->stats.last_report = now;
    float elapsed = (float)(now - p->stats.last_report);
    if (elapsed < 4.5f) return;  /* ~5 second interval */

    fprintf(stderr, "\n===== PERF STATS (%.0fs) =====\n", elapsed);
    fprintf(stderr, "Capture fps:");
    for (int i = 0; i < p->n; i++) {
        fprintf(stderr, " ch%d=%.1f", i, p->stats.cap_frames[i] / elapsed);
    }
    fprintf(stderr, "\nDisplay fps: %.1f\n", p->stats.disp_frames / elapsed);
    fprintf(stderr, "Encode fps:");
    for (int i = 0; i < p->n; i++) {
        if (p->ch[i].enc_ring)
            fprintf(stderr, " ch%d=%.1f", i, p->stats.enc_frames[i] / elapsed);
    }
    fprintf(stderr, "\nInference: total=%.1f fps",
            p->stats.inf_total / elapsed);
    for (int i = 0; i < p->n; i++) {
        fprintf(stderr, " ch%d=%.1f", i, p->stats.inf_per_ch[i] / elapsed);
    }
    fprintf(stderr, "\nInference latency: %.1f ms", p->stats.inf_latency_ms);
    fprintf(stderr, "\nDetections:");
    for (int i = 0; i < p->n; i++) {
        fprintf(stderr, " ch%d=%d", i, p->det_count[i]);
    }
    fprintf(stderr, "\n=================================\n");

    /* Reset counters */
    memset(&p->stats.cap_frames, 0, sizeof(p->stats.cap_frames));
    memset(&p->stats.enc_frames, 0, sizeof(p->stats.enc_frames));
    p->stats.inf_total = 0;
    memset(&p->stats.inf_per_ch, 0, sizeof(p->stats.inf_per_ch));
    p->stats.disp_frames = 0;
    p->stats.last_report = now;
}
```

- [ ] **Step 8: Expose pipe_print_stats in pipeline.h**

Add after `pipe_get_detections`:
```c
void pipe_print_stats(pipeline_t *p);
```

- [ ] **Step 9: Update capture stats in capture_thread**

In capture_thread, after `ch->frame_count++`, add:
```c
        __sync_fetch_and_add(&ch->pipe->stats.cap_frames[ch->id], 1);
```

- [ ] **Step 10: Update encode stats in encode_thread**

In encode_thread, after writing to file, add:
```c
        __sync_fetch_and_add(&ch->pipe->stats.enc_frames[ch->id], 1);
```

- [ ] **Step 11: Build to verify**

Run: `source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1 | tail -5`
Expected: `=== Build OK: rk3568_camera ===`

---

### Task 4: OSD coordinate fix + per-channel display boxes

**Files:**
- Modify: `src/display.h:13` (disp_set_detections gets all, not just cam0)
- Modify: `src/display.c` (per-channel box drawing, coordinate mapping fix)

- [ ] **Step 1: Update display.c OSD drawing to handle per-channel boxes**

Replace the OSD drawing block in `disp_draw()` (lines 474-502) with per-channel support:

```c
    /* Draw detection boxes per channel in its tile */
    pthread_mutex_lock(&d->det_lock);
    glUseProgram(d->osd_prog);
    glLineWidth(3.0f);

    /* Color palette per channel */
    static const float ch_colors[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f},  /* ch0: red */
        {0.0f, 1.0f, 0.0f, 1.0f},  /* ch1: green */
        {0.0f, 0.0f, 1.0f, 1.0f},  /* ch2: blue */
        {1.0f, 1.0f, 0.0f, 1.0f},  /* ch3: yellow */
    };

    int cols = (d->n_cams <= 2) ? d->n_cams : 2;
    int rows = (d->n_cams <= 2) ? 1 : 2;
    float qw = 2.0f / cols, qh = 2.0f / rows;

    for (int cam = 0; cam < d->n_cams && cam < 4; cam++) {
        if (!d->has_frame[cam]) continue;
        if (d->det_count[cam] <= 0) continue;

        /* Tile origin in clip space */
        int col = cam % cols, row = cam / cols;
        float tx0 = -1.0f + col * qw;
        float ty1 =  1.0f - row * qh;   /* top edge */
        /* Scale: original 1920x1080 -> tile space */
        float xs = qw / 1920.0f;
        float ys = qh / 1080.0f;

        glUniform4fv(d->osd_color_loc, 1, ch_colors[cam]);

        for (int i = 0; i < d->det_count[cam]; i++) {
            detection_t *dt = &d->dets[cam][i];
            if (dt->class_id != 0) continue;  /* person only */
            float bx = tx0 + dt->x * xs;
            float by = ty1 - (dt->y + dt->h) * ys;  /* flip Y for GL */
            float bw = dt->w * xs;
            float bh = dt->h * ys;
            float verts[] = { bx,by, bx+bw,by, bx+bw,by+bh, bx,by+bh };
            glVertexAttribPointer(d->osd_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
            glEnableVertexAttribArray(d->osd_pos);
            glDrawArrays(GL_LINE_LOOP, 0, 4);
            glDisableVertexAttribArray(d->osd_pos);
        }
    }
    pthread_mutex_unlock(&d->det_lock);
```

- [ ] **Step 2: Update display struct for per-channel detections**

In `src/display.c`, modify the `display_s` struct (around line 65-67):

Replace:
```c
    detection_t   dets[MAX_DETECTIONS];
    int           det_count;
```
With:
```c
    detection_t   dets[4][MAX_DETECTIONS];
    int           det_count[4];
```

- [ ] **Step 3: Update disp_set_detections for per-channel**

Replace the function (lines 213-220):

```c
void disp_set_detections(display_t *d, const detection_t *dets, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->det_lock);
    /* Reset all channel counts */
    memset(d->det_count, 0, sizeof(d->det_count));
    for (int i = 0; i < n; i++) {
        int cam = dets[i].cam_idx;
        if (cam < 0 || cam >= 4) continue;
        if (d->det_count[cam] >= MAX_DETECTIONS) continue;
        d->dets[cam][d->det_count[cam]++] = dets[i];
    }
    pthread_mutex_unlock(&d->det_lock);
}
```

- [ ] **Step 4: Build to verify**

Run: `source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1 | tail -5`
Expected: `=== Build OK: rk3568_camera ===`

---

### Task 5: main.c config loading and perf stats display

**Files:**
- Modify: `src/main.c` (config loading, perf stats call, display dispatch)

- [ ] **Step 1: Add local config variable declarations and reading**

In the variable declarations area (near line 69-70), add new variables:

```c
    int infer_interval = 5;
    float infer_conf = 0.25f, infer_nms = 0.45f;
    bool  inf_smooth   = true;
    float smooth_a     = 0.6f;
    int   min_persist  = 2;
    bool  inf_rr       = true;
    char  ch_list[16]  = "0,1,2,3";
```

Then in the config reading area (around line 89-91), replace the existing inference section:

```c
        infer_conf = ini_float(cf, "inference", "conf", 0.25f);
        infer_nms  = ini_float(cf, "inference", "nms", 0.45f);
        infer_interval = ini_int(cf, "inference", "interval", 5);
        inf_smooth   = ini_bool(cf, "inference", "smooth_enable", true);
        smooth_a     = ini_float(cf, "inference", "smooth_alpha", 0.6f);
        min_persist  = ini_int(cf, "inference", "min_persist", 2);
        inf_rr       = ini_bool(cf, "inference", "round_robin", true);
        char buf[16];
        char *ch_str = ini_get(cf, "inference", "channels", buf, 16);
        if (ch_str) strncpy(ch_list, ch_str, 15);
```

- [ ] **Step 2: Assign new fields to inf_cfg**

In the `if (model[0])` block (around line 200-209), replace the inf_cfg assignment:

```c
    if (model[0]) {
        inference_cfg_t inf_cfg;
        memset(&inf_cfg, 0, sizeof(inf_cfg));
        strncpy(inf_cfg.model, model, 255);
        inf_cfg.interval      = infer_interval;
        inf_cfg.conf          = infer_conf;
        inf_cfg.nms           = infer_nms;
        inf_cfg.smooth_enable = inf_smooth;
        inf_cfg.smooth_alpha  = smooth_a;
        inf_cfg.min_persist   = min_persist;
        inf_cfg.round_robin   = inf_rr;
        strncpy(inf_cfg.channels, ch_list, 15);
        pipe_set_inference(g_pipe, &inf_cfg);
        printf("[main] inference: %s interval=%d conf=%.2f nms=%.2f "
               "smooth=%d alpha=%.2f persist=%d rr=%d ch=%s\n",
               model, infer_interval, infer_conf, infer_nms,
               inf_smooth, smooth_a, min_persist, inf_rr, ch_list);
    }
```

- [ ] **Step 3: Add perf stats printing to main loop**

In the main loop (around line 239), add stats call:

```c
            struct timeval now; gettimeofday(&now, NULL);
            if ((now.tv_sec-last.tv_sec)*1000000L+(now.tv_usec-last.tv_usec) >= 33000) {
                disp_draw(g_disp); last = now;
                g_pipe->stats.disp_frames++;  /* count display draws */
            }

            /* Print perf stats every ~5 seconds */
            pipe_print_stats(g_pipe);
```

Wait, we need to be careful here. `g_pipe->stats` is not directly accessible because `pipeline_s` is opaque (defined only in pipeline.c). Let me use the public API `pipe_print_stats()` instead, and have it handle the internal timing.

Actually, I already have `pipe_print_stats()` that checks timing internally. Just call it:

In the main loop, add:
```c
            pipe_print_stats(g_pipe);
```

This should be called on each iteration. The function internally checks if 5 seconds have elapsed.

- [ ] **Step 4: Build to verify**

Run: `source /home/rrn/3568/3568_sdk/environment-setup && make clean && make 2>&1 | tail -5`
Expected: `=== Build OK: rk3568_camera ===`

---

### Task 6: Regression test --test-image mode

**Files:**
- Verify: `src/main.c` --test-image path still works

- [ ] **Step 1: Push to device and verify --test-image**

```bash
source /home/rrn/3568/3568_sdk/environment-setup && make clean && make && \
adb push rk3568_camera /userdata/rk3568_camera && \
adb shell chmod +x /userdata/rk3568_camera && \
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib ./rk3568_camera -m /userdata/yolov5.rknn --test-image /userdata/bus.jpg" 2>&1
```

Expected output:
```
[infer] model: input num=1, output num=3
[infer] --- input tensors ---
...
in.type = RKNN_TENSOR_UINT8  (verified via code, not printed)
[main] === RESULTS: 5 detections ===
[main]   [0] cls=0 conf=0.806 box_orig=[110,233,93x295]
[main]   [1] cls=0 conf=0.804 box_orig=[209,243,78x267]
[main]   [2] cls=0 conf=0.620 box_orig=[483,223,81x291]
[main]   [3] cls=5 conf=0.508 box_orig=[116,144,449x309]
[main]   [4] cls=0 conf=0.135 box_orig=[79,333,43x185]
```

Must match the official exactly (same scores, same boxes, same order).

- [ ] **Step 2: Verify single-channel camera + OSD**

```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 15 ./rk3568_camera -m /userdata/yolov5.rknn -c 1 --no-enc 2>&1" | head -30
```

Expected: person detection on cam0, no crashes, smooth box movement.

- [ ] **Step 3: Verify 4-channel round-robin (no display for speed test)**

```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 30 ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc --no-disp 2>&1" | grep -E "PERF STATS|infer.*cam|pipe"
```

Expected: round-robin across channels, inference running at reduced rate but display/capture unaffected.

- [ ] **Step 4: Long run stability test (5 minutes)**

```bash
adb shell "cd /userdata && LD_LIBRARY_PATH=/usr/lib timeout 300 ./rk3568_camera -m /userdata/yolov5.rknn -c 4 --no-enc --no-disp 2>&1" | grep -E "PERF|exit|signal"
```

Expected: clean exit after 300s, no crashes, memory stable.

---

### Task 7: Commit

- [ ] **Step 1: Review all changes**

```bash
cd /home/rrn/rk3568-camera
git diff --stat
```

- [ ] **Step 2: Commit**

```bash
git add -A
git commit -m "feat: add detection smoothing, round-robin inference, OSD fix, perf stats

- detection_t: add cam_idx for per-channel tracking
- smooth.h: EMA box smoothing + persist for OSD (raw RKNN untouched)
- pipeline: round-robin inference across all channels, per-channel dets
- display: per-channel OSD boxes, per-channel colors
- config.ini: smooth_enable, smooth_alpha, min_persist, channels, round_robin
- perf stats: capture/encode/inference fps per channel, latency, printed every 5s"
```
