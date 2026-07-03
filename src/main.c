/* src/main.c - RK3568 4ch AI Camera */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include "pipeline.h"
#include "display.h"
#include "inference.h"
#include <rga/RgaApi.h>
#include <turbojpeg.h>

static pipeline_t *g_pipe = NULL;
static display_t  *g_disp = NULL;
static volatile int g_save_frame = 0;
static int g_save_seq = 0;

static void sigusr_handler(int s)
{
    (void)s;
    g_save_frame = 1;
}

static void sig_handler(int s)
{
    printf("\n[main] signal %d, stopping\n", s);
    if (g_pipe) pipe_stop(g_pipe);
    if (g_disp) disp_close(g_disp);
    exit(0);
}

/* ---- minimal INI parser ---- */
static char *ini_get(FILE *f, const char *section, const char *key, char *val, int sz)
{
    char line[256]; int in_sec = 0;
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '[') {
            char sec[64]; int i = 1;
            while (p[i] && p[i] != ']' && i < 63) { sec[i-1]=p[i]; i++; }
            sec[i-1]=0;
            in_sec = !strcmp(sec, section);
            continue;
        }
        if (!in_sec || p[0]=='#' || p[0]=='\n') continue;
        if (strncmp(p, key, strlen(key))==0) {
            p += strlen(key);
            while (*p==' '||*p=='\t'||*p=='='||*p==':') p++;
            int i=0;
            while (p[i]&&p[i]!='\n'&&p[i]!='#'&&i<sz-1) val[i]=p[i], i++;
            while (i>0&&(val[i-1]==' '||val[i-1]=='\t')) i--;
            val[i]=0;
            return val;
        }
    }
    return NULL;
}
static int ini_int(FILE *f, const char *s, const char *k, int d) {
    char b[64]; return ini_get(f,s,k,b,64) ? atoi(b) : d;
}
static float ini_float(FILE *f, const char *s, const char *k, float d) {
    char b[64]; return ini_get(f,s,k,b,64) ? atof(b) : d;
}
static int ini_bool(FILE *f, const char *s, const char *k, int d) {
    char b[16]; char *v = ini_get(f,s,k,b,16);
    return v ? (!strcmp(v,"true")||!strcmp(v,"1")) : d;
}

int main(int argc, char **argv)
{
    int n_cams = 4, w = 1920, h = 1080, fps = 25, max_frames = 0;
    int cam_offset = 0, use_disp = 1, use_enc = 1, codec_h265 = 1;
    char out_pat[256] = "/tmp/cam_%d.h264", model[256] = "";
    int infer_interval = 5;
    float infer_conf = 0.25f, infer_nms = 0.45f;
    bool  inf_person_only = true;
    float person_conf     = 0.10f;
    bool  inf_smooth      = true;
    float smooth_a        = 0.6f;
    int   min_persist     = 2;
    bool  inf_rr          = true;
    bool  inf_rga         = false;
    char  ch_list[16]     = "0,1,2,3";
    char test_image[256] = {0};   /* --test-image path */

    /* Load config */
    const char *config_path = getenv("RK3568_CONFIG");
    if (!config_path || !config_path[0])
        config_path = "/userdata/rk3568-camera/config.ini";
    FILE *cf = fopen(config_path, "r");
    if (cf) {
        n_cams  = ini_int(cf, "camera", "count", n_cams);
        w       = ini_int(cf, "camera", "width", w);
        h       = ini_int(cf, "camera", "height", h);
        fps     = ini_int(cf, "camera", "fps", fps);
        char cc[8] = {0};
        ini_get(cf, "encoder", "codec", cc, 8);
        codec_h265 = (cc[0]==0 || strcmp(cc,"h265")==0);
        ini_get(cf, "output", "pattern", out_pat, 256);
        max_frames = ini_int(cf, "output", "frames", 0);
        use_disp = ini_bool(cf, "display", "enabled", 1);
        int inf_en = ini_bool(cf, "inference", "enabled", 0);
        if (inf_en) ini_get(cf, "inference", "model", model, 256);
        infer_interval = ini_int(cf, "inference", "interval", 1);
        infer_conf = ini_float(cf, "inference", "conf", 0.25f);
        infer_nms  = ini_float(cf, "inference", "nms", 0.45f);
        inf_person_only   = ini_bool(cf, "inference", "person_only", true);
        person_conf       = ini_float(cf, "inference", "person_conf", 0.10f);
        inf_smooth        = ini_bool(cf, "inference", "smooth_enable", true);
        smooth_a          = ini_float(cf, "inference", "smooth_alpha", 0.6f);
        min_persist       = ini_int(cf, "inference", "min_persist", 2);
        inf_rr            = ini_bool(cf, "inference", "round_robin", true);
        inf_rga           = ini_bool(cf, "inference", "rga_preprocess", false);
        {
            char buf[16];
            char *ch = ini_get(cf, "inference", "channels", buf, 16);
            if (ch) strncpy(ch_list, ch, 15);
        }
        fclose(cf);
    }

    /* CLI overrides */
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"-c")&&i+1<argc) n_cams=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--cam")&&i+1<argc) cam_offset=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-n")&&i+1<argc) max_frames=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--no-enc")) use_enc=0;
        else if (!strcmp(argv[i],"--no-disp")) use_disp=0;
        else if (!strcmp(argv[i],"-w")&&i+1<argc) w=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-h")&&i+1<argc) h=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-m")&&i+1<argc) strncpy(model,argv[++i],255);
        else if (!strcmp(argv[i],"--test-image")&&i+1<argc) strncpy(test_image,argv[++i],255);
        else if (!strcmp(argv[i],"--rga")) inf_rga = true;
    }

    /* Check NPU frequency (only meaningful when inference is enabled) */
    if (model[0]) {
        const char *gov_path = "/sys/class/devfreq/fde40000.npu/governor";
        const char *freq_path = "/sys/devices/platform/fde40000.npu/devfreq/fde40000.npu/cur_freq";
        FILE *fg = fopen(gov_path, "r");
        if (fg) {
            char gov[32] = {0};
            fgets(gov, sizeof(gov), fg); fclose(fg);
            for (int i = 0; gov[i]; i++) if (gov[i] == '\n') gov[i] = 0;
            fprintf(stderr, "[main] NPU governor: %s\n", gov);
        }
        FILE *ff = fopen(freq_path, "r");
        if (ff) {
            long freq = 0;
            fscanf(ff, "%ld", &freq); fclose(ff);
            fprintf(stderr, "[main] NPU frequency: %ld Hz\n", freq);
            if (freq > 0 && freq < 800000000) {
                fprintf(stderr, "[main] WARNING: NPU freq %ld Hz is low. "
                        "Expected >=800MHz for realtime 320 model.\n"
                        "[main] Run: echo performance > %s\n",
                        freq, gov_path);
            }
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sigusr_handler);

    /* ======== File test-image mode (bypass camera pipeline) ======== */
    if (test_image[0]) {
        if (!model[0]) {
            fprintf(stderr, "[main] --test-image requires -m <model.rknn>\n");
            return 1;
        }
        printf("[main] === FILE TEST MODE ===\n");
        printf("[main] image=%s model=%s conf=%.2f nms=%.2f\n",
               test_image, model, infer_conf, infer_nms);

        /* Read JPEG */
        FILE *jf = fopen(test_image, "rb");
        if (!jf) { perror(test_image); return 1; }
        fseek(jf, 0, SEEK_END);
        long jsz = ftell(jf);
        fseek(jf, 0, SEEK_SET);
        unsigned char *jbuf = (unsigned char *)malloc(jsz);
        fread(jbuf, 1, jsz, jf);
        fclose(jf);

        tjhandle tj = tjInitDecompress();
        int jw, jh, subsamp, cs;
        tjDecompressHeader3(tj, jbuf, jsz, &jw, &jh, &subsamp, &cs);
        printf("[main] JPEG: %dx%d subsamp=%d colorspace=%d\n", jw, jh, subsamp, cs);

        unsigned char *rgb = (unsigned char *)malloc(jw * jh * 3);
        if (tjDecompress2(tj, jbuf, jsz, rgb, jw, 0, jh, TJPF_RGB, 0) < 0) {
            fprintf(stderr, "[main] JPEG decode error: %s\n", tjGetErrorStr());
            free(rgb); free(jbuf); tjDestroy(tj);
            return 1;
        }
        tjDestroy(tj);
        free(jbuf);

        /* Open inference */
        infer_t *inf = infer_open(model, infer_conf, infer_nms, false);
        if (!inf) { fprintf(stderr, "[main] infer_open failed\n"); free(rgb); return 1; }

        /* Run detection */
        detection_t dets[MAX_DETECTIONS];
        int nd = infer_detect_rgb(inf, rgb, jw, jh, dets, MAX_DETECTIONS);

        printf("[main] === RESULTS: %d detections ===\n", nd);
        for (int i = 0; i < nd; i++) {
            printf("[main]   [%d] cls=%d conf=%.3f box_orig=[%d,%d,%dx%d]\n",
                   i, dets[i].class_id, dets[i].confidence,
                   dets[i].x, dets[i].y, dets[i].w, dets[i].h);
        }

        /* Also dump PPM of the decoded JPEG for comparison */
        {
            FILE *fp = fopen("/tmp/test_input_orig.ppm", "wb");
            if (fp) {
                fprintf(fp, "P6\n%d %d\n255\n", jw, jh);
                fwrite(rgb, 1, jw * jh * 3, fp);
                fclose(fp);
                printf("[main] wrote /tmp/test_input_orig.ppm (%dx%d)\n", jw, jh);
            }
        }

        free(rgb);
        infer_close(inf);
        printf("[main] file test done\n");
        return 0;
    }

    /* Display */
    if (use_disp) {
        g_disp = disp_open(1280, 720, n_cams);
        if (!g_disp) { fprintf(stderr, "[main] display failed\n"); return 1; }
    }

    /* Build configs */
    capture_cfg_t cam_cfg[4];
    for (int i=0; i<n_cams; i++) {
        snprintf(cam_cfg[i].device, 64, "/dev/video%d", cam_offset + i);
        cam_cfg[i].width=w; cam_cfg[i].height=h; cam_cfg[i].fps=fps;
        cam_cfg[i].format = 0x3231564e;
    }
    encoder_cfg_t enc_cfg = {w, h, fps, 4000000, fps, ""};
    strncpy(enc_cfg.codec, codec_h265 ? "h265" : "h264", 7);

    /* Pipeline */
    g_pipe = pipe_new(n_cams, cam_cfg, use_enc?&enc_cfg:NULL, max_frames);

    /* Init RGA (only when needed for RGA-based NV12→RGB preprocess) */
    if (model[0] && inf_rga) {
        c_RkRgaInit();
    }

    /* Set up inference BEFORE pipe_start (threads are spawned in pipe_start) */
    if (model[0]) {
        inference_cfg_t inf_cfg;
        memset(&inf_cfg, 0, sizeof(inf_cfg));
        strncpy(inf_cfg.model, model, 255);
        inf_cfg.interval      = infer_interval;
        inf_cfg.conf          = infer_conf;
        inf_cfg.nms           = infer_nms;
        inf_cfg.person_only   = inf_person_only;
        inf_cfg.person_conf   = person_conf;
        inf_cfg.smooth_enable = inf_smooth;
        inf_cfg.smooth_alpha  = smooth_a;
        inf_cfg.min_persist   = min_persist;
        inf_cfg.round_robin   = inf_rr;
        inf_cfg.rga_preprocess = inf_rga;
        strncpy(inf_cfg.channels, ch_list, 15);
        pipe_set_inference(g_pipe, &inf_cfg);
        printf("[main] inference: %s interval=%d conf=%.2f nms=%.2f "
               "person_only=%d person_conf=%.2f "
               "smooth=%d alpha=%.2f persist=%d rr=%d ch=%s rga=%d\n",
               model, infer_interval, infer_conf, infer_nms,
               inf_person_only, person_conf,
               inf_smooth, smooth_a, min_persist, inf_rr, ch_list, inf_rga);
    }

    if (pipe_start(g_pipe) < 0) {
        fprintf(stderr, "[main] pipeline failed\n");
        if (g_disp) disp_close(g_disp);
        return 1;
    }

    printf("[main] %d cams %dx%d@%d %s disp=%d enc=%d\n",
           n_cams, w, h, fps, codec_h265?"H.265":"H.264", use_disp, use_enc);

    /* Main loop */
    struct timeval last = {0};
    struct timeval last_snap = {0};
    gettimeofday(&last_snap, NULL);
    int snap_interval = 2;  /* auto-save every 2 seconds */
    int total_snaps = 0;
    int max_snaps = 0;
    printf("[main] auto-save every %ds, max %d frames per camera\n",
           snap_interval, max_snaps);
    while (1) {
        /* Auto-save check: trigger every snap_interval seconds */
        if (total_snaps < max_snaps) {
            struct timeval now_snap;
            gettimeofday(&now_snap, NULL);
            long elapsed = (now_snap.tv_sec - last_snap.tv_sec) * 1000000L
                         + (now_snap.tv_usec - last_snap.tv_usec);
            if (elapsed >= snap_interval * 1000000L) {
                g_save_frame = 1;
                last_snap = now_snap;
                printf("[main] auto-snap %d/%d\n", total_snaps + 1, max_snaps);
                total_snaps++;
            }
        }
        if (g_disp) {
            disp_dispatch(g_disp);
            /* Push detections to display overlay */
            detection_t dets[MAX_DETECTIONS];
            int nd = pipe_get_detections(g_pipe, dets, MAX_DETECTIONS);
            disp_set_detections(g_disp, dets, nd);
            for (int i=0; i<n_cams; i++) {
                ring_t *r = pipe_display_ring(g_pipe, i);
                frame_t f;
                if (ring_get(r, &f)) {
                    disp_update(g_disp, i, &f);
                    /* SIGUSR1: save raw NV12 frame to /tmp/ */
                    if (g_save_frame && f.ptr) {
                        char path[64];
                        snprintf(path, sizeof(path),
                                 "/tmp/chess_cam%d_%02d.nv12", cam_offset + i, g_save_seq);
                        FILE *fp = fopen(path, "wb");
                        if (fp) {
                            uint8_t *s = f.ptr;
                            for (int r = 0; r < f.height; r++) {
                                fwrite(s, 1, f.width, fp);
                                s += f.stride;
                            }
                            s = (uint8_t*)f.ptr + f.stride * f.height;
                            for (int r = 0; r < f.height / 2; r++) {
                                fwrite(s, 1, f.width, fp);
                                s += f.stride;
                            }
                            fclose(fp);
                            printf("[save] %s (%dx%d)\n", path, f.width, f.height);
                        }
                        /* Reset after saving all active cameras (last cam) */
                        if (i == n_cams - 1) {
                            g_save_frame = 0;
                            g_save_seq++;
                        }
                    }
                    frame_release(&f);
                }
            }
            struct timeval now; gettimeofday(&now, NULL);
            if ((now.tv_sec-last.tv_sec)*1000000L+(now.tv_usec-last.tv_usec) >= 33000) {
                disp_draw(g_disp); last = now;
                pipe_stats_disp_tick(g_pipe);
            }
        } else {
            usleep(33000);
        }
        pipe_print_stats(g_pipe);

        if (max_frames > 0 && pipe_done(g_pipe)) {
            printf("[main] done\n"); break;
        }
    }

    if (g_pipe) pipe_stop(g_pipe);
    if (g_disp) disp_close(g_disp);
    return 0;
}
