/* pipeline.c - capture threads (light) + encode threads (per camera) */
#include "pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "inference.h"
#include "smooth.h"
#include <sys/time.h>
#include <time.h>

typedef struct {
    int            id;
    capture_cfg_t  cfg;
    capture_t     *cap;
    encoder_t     *enc;
    ring_t        *disp_ring;
    ring_t        *enc_ring;
    ring_t        *infer_ring;
    pipeline_t    *pipe;
    pthread_t      cap_thr, enc_thr;
    volatile bool  running;
    int            frame_count;
} channel_t;

struct pipeline_s {
    channel_t         ch[MAX_CAMS];
    ring_t            disp_rings[MAX_CAMS];
    ring_t            enc_rings[MAX_CAMS];
    ring_t            infer_rings[MAX_CAMS];
    encoder_cfg_t    *enc_cfg;
    inference_cfg_t  *inf_cfg;
    detection_t       dets[MAX_CAMS][MAX_DETECTIONS];
    int               det_count[MAX_CAMS];
    smooth_state_t    smooth[MAX_CAMS];
    pthread_mutex_t   det_lock;
    pthread_t         infer_thr;
    int               n, max_frames;
    volatile bool     running;
    struct {
        int      cap_frames[MAX_CAMS];
        int      enc_frames[MAX_CAMS];
        int      inf_total;
        int      inf_per_ch[MAX_CAMS];
        float    inf_latency_ms;
        float    box_age_ms;
        float    box_age_max;
        int      dropped_frames;
        int      inf_frame_seq;
        int      disp_frames;
        time_t   last_report;
    } stats;
};

/* ---- capture thread: DQBUF → push rings → QBUF (no memcpy, no encode) ---- */
static void *capture_thread(void *arg)
{
    channel_t *ch = arg;
    frame_t f = {0};
    f.cam_idx = ch->id;
    static volatile uint32_t g_cap_seq = 0;

    while (ch->running) {
        if (cap_dequeue(ch->cap, &f) < 0) continue;
        ch->frame_count++;
        f.cap_seq = __sync_fetch_and_add(&g_cap_seq, 1);
        __sync_fetch_and_add(&ch->pipe->stats.cap_frames[ch->id], 1);

        ring_put(ch->disp_ring, &f);          /* shallow copy to display */
        if (ch->enc_ring)
            ring_put(ch->enc_ring, &f);        /* shallow copy to encoder */

        if (ch->infer_ring && ch->pipe->inf_cfg &&
            (ch->frame_count % ch->pipe->inf_cfg->interval == 0)) {
            /* Deep copy NV12 — V4L2 buffer may be overwritten after QBUF */
            frame_t fcopy = f;
            int ysz = f.width * f.height, uvsz = ysz / 2;
            fcopy.ptr = malloc(ysz + uvsz);
            if (fcopy.ptr) {
                /* Copy Y plane row-by-row (may have stride padding) */
                uint8_t *d = fcopy.ptr, *s = f.ptr;
                for (int r = 0; r < f.height; r++)
                    { memcpy(d, s, f.width); s += f.stride; d += f.width; }
                /* Copy UV plane */
                s = (uint8_t*)f.ptr + f.stride * f.height;
                for (int r = 0; r < f.height/2; r++)
                    { memcpy(d, s, f.width); s += f.stride; d += f.width; }
                /* Timestamp for age tracking (overwrite V4L2 monotonic with realtime us) */
                struct timeval tv;
                gettimeofday(&tv, NULL);
                fcopy.pts = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
                fcopy.seq = ch->frame_count;
                fcopy.own_ptr = true;  /* deep copy — consumer must free */
                /* Preserve dma_buf fd for RGA preprocess — dup for infer ring */
                fcopy.fd = (f.fd >= 0) ? dup(f.fd) : -1;
                ring_put(ch->infer_ring, &fcopy);
            }
        }
        if (f.fd >= 0) close(f.fd);            /* we dup'd it; rings have their own */
        cap_queue(ch->cap);

        if (ch->pipe->max_frames > 0 &&
            ch->frame_count >= ch->pipe->max_frames) break;
    }
    printf("[ch%d] exit: %d frames\n", ch->id, ch->frame_count);
    return NULL;
}

/* ---- encode thread: pull from ring → MPP → write file ---- */
static void *encode_thread(void *arg)
{
    channel_t *ch = arg;
    char path[256];
    FILE *fp = NULL;

    snprintf(path, sizeof(path), "/tmp/cam_%d.h264", ch->id);
    fp = fopen(path, "wb");
    printf("[enc%d] writing to %s\n", ch->id, path);

    while (ch->running) {
        frame_t f;
        if (!ring_get(ch->enc_ring, &f)) {
            usleep(5000);  /* no frame, wait 5ms */
            continue;
        }

        uint8_t *out = NULL; size_t len = 0;
        enc_feed(ch->enc, &f, &out, &len);

        if (out && len > 0 && fp) {
            uint8_t sc[4] = {0,0,0,1};
            fwrite(sc, 1, 4, fp);
            fwrite(out, 1, len, fp);
        }
        free(out);
        if (f.fd >= 0) close(f.fd);
        __sync_fetch_and_add(&ch->pipe->stats.enc_frames[ch->id], 1);
    }

    if (fp) fclose(fp);
    printf("[enc%d] exit\n", ch->id);
    return NULL;
}

static void *infer_thread(void *arg)
{
    pipeline_t *p = arg;
    infer_t *inf = infer_open(p->inf_cfg->model, p->inf_cfg->conf, p->inf_cfg->nms,
                               p->inf_cfg->rga_preprocess);
    if (!inf) { fprintf(stderr, "[infer] model load failed\n"); return NULL; }

    /* Init per-channel smooth state */
    for (int i = 0; i < MAX_CAMS; i++) {
        smooth_init(&p->smooth[i], p->inf_cfg->smooth_alpha, p->inf_cfg->min_persist);
    }

    /* Parse channel list: "0,1,2,3" */
    int ch_list[MAX_CAMS] = {0};
    int n_ch = 0;
    {
        const char *s = p->inf_cfg->channels;
        while (*s && n_ch < MAX_CAMS) {
            while (*s == ' ' || *s == ',') s++;
            if (*s >= '0' && *s <= '9') {
                ch_list[n_ch++] = atoi(s);
                while (*s >= '0' && *s <= '9') s++;
            } else if (*s) s++;
        }
    }
    if (n_ch == 0) { ch_list[0] = 0; n_ch = 1; }

    int current_ch = 0;
    struct timeval t_start, t_end;

    while (p->running) {
        int cam = ch_list[current_ch];
        frame_t f;
        if (!ring_get(&p->infer_rings[cam], &f)) {
            current_ch = (current_ch + 1) % n_ch;
            usleep(1000);
            continue;
        }

        gettimeofday(&t_start, NULL);
        detection_t raw_dets[MAX_DETECTIONS];
        int n_raw = infer_detect(inf, &f, raw_dets, MAX_DETECTIONS);
        gettimeofday(&t_end, NULL);

        for (int i = 0; i < n_raw; i++)
            raw_dets[i].cam_idx = cam;

        float lat_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0f +
                       (t_end.tv_usec - t_start.tv_usec) / 1000.0f;

        /* Box age: time from V4L2 capture (f.pts in us) to inference end */
        float age_ms = (t_end.tv_sec * 1000000LL + t_end.tv_usec - f.pts) / 1000.0f;

        /* Person-only: extract cls=0 above person_conf threshold */
        float p_thresh = p->inf_cfg->person_conf;
        detection_t person_cand[MAX_DETECTIONS];
        int n_cand = 0;
        float best_score = 0.f;
        for (int i = 0; i < n_raw; i++) {
            if (raw_dets[i].class_id != 0) continue;
            if (raw_dets[i].confidence > best_score)
                best_score = raw_dets[i].confidence;
            if (raw_dets[i].confidence >= p_thresh)
                person_cand[n_cand++] = raw_dets[i];
        }

        /* Smooth (person-only, cls=0 already ensured) */
        detection_t smooth_dets[MAX_DETECTIONS];
        int n_drawn = 0;
        if (p->inf_cfg->smooth_enable) {
            n_drawn = smooth_update(&p->smooth[cam], person_cand, n_cand,
                                     smooth_dets, MAX_DETECTIONS);
        } else {
            n_drawn = (n_cand > MAX_DETECTIONS) ? MAX_DETECTIONS : n_cand;
            if (n_drawn > 0)
                memcpy(smooth_dets, person_cand, n_drawn * sizeof(detection_t));
        }

        pthread_mutex_lock(&p->det_lock);
        p->det_count[cam] = n_drawn;
        if (n_drawn > 0)
            memcpy(p->dets[cam], smooth_dets, n_drawn * sizeof(detection_t));
        else
            p->det_count[cam] = 0;
        pthread_mutex_unlock(&p->det_lock);

        p->stats.inf_total++;
        p->stats.inf_per_ch[cam]++;
        p->stats.inf_latency_ms = lat_ms;

        /* Stdout summary for quick visual check */
        if (n_drawn > 0) {
            printf("[DETECT] cam%d person=%d best=%.2f box=(%d,%d,%dx%d) age=%.0fms\n",
                   cam, n_drawn, best_score,
                   smooth_dets[0].x, smooth_dets[0].y,
                   smooth_dets[0].w, smooth_dets[0].h, age_ms);
        }
        p->stats.box_age_ms = age_ms;
        if (age_ms > p->stats.box_age_max) p->stats.box_age_max = age_ms;
        p->stats.inf_frame_seq = f.seq;

        /* Person-only logging with age + freshness */
        fprintf(stderr, "[infer] cam%d cap_seq=%u seq=%u person=%d/%d best=%.3f age=%.0fms "
                "lat=%.1fms raw=%d\n",
                cam, f.cap_seq, f.seq, n_drawn, n_cand, best_score, age_ms, lat_ms, n_raw);

        current_ch = (current_ch + 1) % n_ch;
        if (f.fd >= 0) close(f.fd);
        free(f.ptr);
    }

    /* Drain remaining frames from all infer rings */
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

/* ---- public API ---- */

pipeline_t *pipe_new(int n, capture_cfg_t *cam, encoder_cfg_t *enc, int max_frames)
{
    pipeline_t *p = calloc(1, sizeof(*p));
    p->n = (n > MAX_CAMS) ? MAX_CAMS : n;
    p->max_frames = max_frames;
    p->enc_cfg = enc;
    p->inf_cfg = NULL;
    pthread_mutex_init(&p->det_lock, NULL);
    for (int i = 0; i < p->n; i++) {
        p->ch[i].id = i; p->ch[i].cfg = cam[i]; p->ch[i].pipe = p;
        p->ch[i].disp_ring = &p->disp_rings[i];
        if (enc) p->ch[i].enc_ring = &p->enc_rings[i];
        p->ch[i].infer_ring = &p->infer_rings[i];
    }
    return p;
}

int pipe_start(pipeline_t *p)
{
    p->running = true;
    for (int i = 0; i < p->n; i++) {
        channel_t *ch = &p->ch[i];
        ch->cap = cap_open(ch->cfg.device, ch->cfg.width,
                           ch->cfg.height, ch->cfg.fps, ch->cfg.format);
        if (!ch->cap) { pipe_stop(p); return -1; }

        if (ch->enc_ring) {
            ch->enc = enc_open(ch->cfg.width, ch->cfg.height,
                               ch->cfg.fps, p->enc_cfg->bitrate,
                               p->enc_cfg->codec);
        }
    }

    /* Start encode threads first (they'll wait for frames) */
    for (int i = 0; i < p->n; i++) {
        p->ch[i].running = true;
        if (p->ch[i].enc_ring)
            pthread_create(&p->ch[i].enc_thr, NULL, encode_thread, &p->ch[i]);
    }
    if (p->inf_cfg) {
        pthread_create(&p->infer_thr, NULL, infer_thread, p);
    }
    /* Then capture threads */
    for (int i = 0; i < p->n; i++)
        pthread_create(&p->ch[i].cap_thr, NULL, capture_thread, &p->ch[i]);

    printf("[pipe] %d cameras started%s\n", p->n, p->enc_cfg ? " + encode" : "");
    return 0;
}

void pipe_stop(pipeline_t *p)
{
    if (!p) return;
    p->running = false;
    if (p->infer_thr) pthread_join(p->infer_thr, NULL);
    for (int i = 0; i < p->n; i++) {
        p->ch[i].running = false;
        if (p->ch[i].cap_thr) pthread_join(p->ch[i].cap_thr, NULL);
        if (p->ch[i].enc_thr) pthread_join(p->ch[i].enc_thr, NULL);
        if (p->ch[i].enc) enc_close(p->ch[i].enc);
        if (p->ch[i].cap) cap_close(p->ch[i].cap);
    }
}

bool pipe_done(pipeline_t *p)
{
    if (!p || p->max_frames <= 0) return false;
    for (int i = 0; i < p->n; i++)
        if (p->ch[i].frame_count < p->max_frames) return false;
    return true;
}

ring_t *pipe_display_ring(pipeline_t *p, int cam_idx)
{
    if (!p || cam_idx < 0 || cam_idx >= p->n) return NULL;
    return &p->disp_rings[cam_idx];
}

void pipe_set_inference(pipeline_t *p, inference_cfg_t *cfg)
{
    if (!p || !cfg) return;
    if (!p->inf_cfg) p->inf_cfg = malloc(sizeof(inference_cfg_t));
    if (p->inf_cfg) memcpy(p->inf_cfg, cfg, sizeof(inference_cfg_t));
}

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

void pipe_stats_disp_tick(pipeline_t *p)
{
    if (!p) return;
    __sync_fetch_and_add(&p->stats.disp_frames, 1);
}

void pipe_print_stats(pipeline_t *p)
{
    if (!p) return;
    time_t now = time(NULL);
    if (p->stats.last_report == 0) p->stats.last_report = now;
    float elapsed = (float)(now - p->stats.last_report);
    if (elapsed < 4.5f) return;

    fprintf(stderr, "\n===== PERF (%.0fs) =====\n", elapsed);
    fprintf(stderr, "Capture fps:");
    for (int i = 0; i < p->n; i++)
        fprintf(stderr, " ch%d=%.1f", i, p->stats.cap_frames[i] / elapsed);
    fprintf(stderr, "\nDisplay fps: %.1f", p->stats.disp_frames / elapsed);
    fprintf(stderr, "\nEncode fps:");
    for (int i = 0; i < p->n; i++)
        if (p->ch[i].enc_ring)
            fprintf(stderr, " ch%d=%.1f", i, p->stats.enc_frames[i] / elapsed);
    fprintf(stderr, "\nInference: total=%.1f/s latency=%.1fms",
            p->stats.inf_total / elapsed, p->stats.inf_latency_ms);
    for (int i = 0; i < p->n; i++)
        fprintf(stderr, " ch%d=%.1f", i, p->stats.inf_per_ch[i] / elapsed);
    fprintf(stderr, "\nBox age: avg=%.0fms max=%.0fms frame_seq=%d",
            p->stats.box_age_ms, p->stats.box_age_max, p->stats.inf_frame_seq);
    fprintf(stderr, "\nDetections:");
    for (int i = 0; i < p->n; i++)
        fprintf(stderr, " ch%d=%d", i, p->det_count[i]);
    fprintf(stderr, "\n========================\n");

    p->stats.box_age_max = 0.f;

    memset(&p->stats.cap_frames, 0, sizeof(p->stats.cap_frames));
    memset(&p->stats.enc_frames, 0, sizeof(p->stats.enc_frames));
    p->stats.inf_total = 0;
    memset(&p->stats.inf_per_ch, 0, sizeof(p->stats.inf_per_ch));
    p->stats.disp_frames = 0;
    p->stats.last_report = now;
}
