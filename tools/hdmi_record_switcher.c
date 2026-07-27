#include <errno.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  char root[512];
  int cam;
  int segment_sec;
  time_t base_time;
  char latest[512];
} PathPattern;

typedef struct {
  GstElement *pipeline;
  GstElement *selector;
  GstPad *pad0;
  GstPad *pad1;
  GMainLoop *loop;
  gboolean eos_sent;
  int evfds[16];
  int evfd_count;
  int stdin_fd;
  struct termios old_termios;
  gboolean termios_set;
  char latest0[512];
  char latest1[512];
} App;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static int env_int(const char *name, int def) {
  const char *v = getenv(name);
  if (!v || !*v) return def;
  return atoi(v);
}

static const char *env_str(const char *name, const char *def) {
  const char *v = getenv(name);
  return (v && *v) ? v : def;
}

static void setup_record_timezone(void) {
  const char *tz = env_str("HDMI_REC_TZ", "CST-8");
  setenv("TZ", tz, 1);
  tzset();
}

static int mkdir_p(const char *path) {
  char tmp[512];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) return -1;
  strcpy(tmp, path);
  for (char *p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
  return 0;
}

static int is_mountpoint(const char *path) {
  FILE *fp = fopen("/proc/mounts", "r");
  if (!fp) return 0;
  char dev[256], mount[256], type[64], opts[256];
  int found = 0;
  while (fscanf(fp, "%255s %255s %63s %255s %*d %*d\n", dev, mount, type, opts) == 4) {
    if (strcmp(mount, path) == 0) {
      found = 1;
      break;
    }
  }
  fclose(fp);
  return found;
}

static int setup_stdin(App *app) {
  app->stdin_fd = STDIN_FILENO;
  if (!isatty(app->stdin_fd)) return 0;
  if (tcgetattr(app->stdin_fd, &app->old_termios) != 0) return -1;
  struct termios raw = app->old_termios;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(app->stdin_fd, TCSANOW, &raw) != 0) return -1;
  app->termios_set = TRUE;
  return 0;
}

static void restore_stdin(App *app) {
  if (app->termios_set) {
    tcsetattr(app->stdin_fd, TCSANOW, &app->old_termios);
  }
}

static void switch_to(App *app, int cam) {
  GstPad *pad = cam == 1 ? app->pad1 : app->pad0;
  g_object_set(app->selector, "active-pad", pad, NULL);
  fprintf(stderr, "[switch] HDMI cam%d\n", cam);
}

static void request_stop(App *app) {
  if (app->eos_sent) return;
  app->eos_sent = TRUE;
  fprintf(stderr, "[record] stopping, finalizing MP4 files...\n");
  gst_element_send_event(app->pipeline, gst_event_new_eos());
}

static void print_playback(App *app) {
  fprintf(stderr, "[playback] latest cam0: %s\n", app->latest0[0] ? app->latest0 : "(none yet)");
  fprintf(stderr, "[playback] latest cam1: %s\n", app->latest1[0] ? app->latest1 : "(none yet)");
  fprintf(stderr, "[playback] playback UI is reserved; recording continues.\n");
}

static gboolean on_io(GIOChannel *source, GIOCondition cond, gpointer data) {
  App *app = (App *)data;
  (void)source;
  if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) return TRUE;
  char ch;
  while (read(STDIN_FILENO, &ch, 1) == 1) {
    if (ch == '1') switch_to(app, 0);
    else if (ch == '2') switch_to(app, 1);
    else if (ch == '3') print_playback(app);
    else if (ch == 'q' || ch == 'Q') {
      request_stop(app);
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean on_event(GIOChannel *source, GIOCondition cond, gpointer data) {
  App *app = (App *)data;
  if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) return TRUE;
  int fd = g_io_channel_unix_get_fd(source);
  struct input_event ev;
  while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    if (ev.type != EV_KEY || ev.value != 1) continue;
    fprintf(stderr, "[input] key code=%u\n", ev.code);
    if (ev.code == KEY_1 || ev.code == KEY_KP1) switch_to(app, 0);
    else if (ev.code == KEY_2 || ev.code == KEY_KP2) switch_to(app, 1);
    else if (ev.code == KEY_3 || ev.code == KEY_KP3) print_playback(app);
    else if (ev.code == KEY_Q || ev.code == KEY_ESC) {
      request_stop(app);
      return FALSE;
    }
  }
  return TRUE;
}

static gboolean check_signal(gpointer data) {
  App *app = (App *)data;
  if (g_stop) {
    request_stop(app);
  }
  return TRUE;
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data) {
  App *app = (App *)data;
  (void)bus;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      fprintf(stderr, "[gst:error] %s\n", err ? err->message : "unknown");
      if (dbg) fprintf(stderr, "[gst:debug] %s\n", dbg);
      if (err) g_error_free(err);
      g_free(dbg);
      g_main_loop_quit(app->loop);
      break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit(app->loop);
      break;
    default:
      break;
  }
  return TRUE;
}

static gchar *on_format_location(GstElement *splitmux, guint fragment_id, gpointer user_data) {
  (void)splitmux;
  (void)fragment_id;
  PathPattern *pattern = (PathPattern *)user_data;
  time_t start = pattern->base_time + (time_t)fragment_id * pattern->segment_sec;
  time_t end = start + pattern->segment_sec;
  struct tm start_tm;
  struct tm end_tm;
  char date_dir[32];
  char start_name[32];
  char end_name[32];
  char dir[512];

  localtime_r(&start, &start_tm);
  localtime_r(&end, &end_tm);
  strftime(date_dir, sizeof(date_dir), "%Y-%m-%d", &start_tm);
  strftime(start_name, sizeof(start_name), "%H-%M-%S", &start_tm);
  strftime(end_name, sizeof(end_name), "%H-%M-%S", &end_tm);

  snprintf(dir, sizeof(dir), "%s/cam%d/%s", pattern->root, pattern->cam, date_dir);
  if (mkdir_p(dir) != 0) {
    fprintf(stderr, "[record] cannot create output dir: %s\n", dir);
  }
  snprintf(pattern->latest, sizeof(pattern->latest), "%s/%s_%s.mp4", dir, start_name, end_name);
  return g_strdup(pattern->latest);
}

static void open_event_devices(App *app, const char *event_devs) {
  if (!event_devs || !*event_devs) return;

  char buf[512];
  snprintf(buf, sizeof(buf), "%s", event_devs);
  for (char *tok = strtok(buf, " ,:;"); tok && app->evfd_count < 16; tok = strtok(NULL, " ,:;")) {
    int fd = open(tok, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      fprintf(stderr, "[input] cannot open %s: %s\n", tok, strerror(errno));
      continue;
    }
    app->evfds[app->evfd_count++] = fd;
    GIOChannel *ev_ch = g_io_channel_unix_new(fd);
    g_io_add_watch(ev_ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_event, app);
    g_io_channel_unref(ev_ch);
    fprintf(stderr, "[input] reading %s\n", tok);
  }
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  setup_record_timezone();

  int width = env_int("HDMI_REC_WIDTH", 1920);
  int height = env_int("HDMI_REC_HEIGHT", 1080);
  int fps = env_int("HDMI_REC_FPS", 15);
  int bitrate = env_int("HDMI_REC_BITRATE", 4000000);
  int crop_percent = env_int("HDMI_REC_CROP_PERCENT", 80);
  int segment_sec = env_int("HDMI_REC_SEGMENT_SEC", 60);
  int max_files = env_int("HDMI_REC_MAX_FILES", 0);
  const char *root = env_str("HDMI_REC_ROOT", "/mnt/sdcard/rk3568_recordings");
  const char *sd_mount = env_str("HDMI_REC_SD_MOUNT", "/mnt/sdcard");
  const char *event_dev = env_str("HDMI_REC_EVENT", "");

  if (!is_mountpoint(sd_mount)) {
    fprintf(stderr, "[record] SD mount not found: %s\n", sd_mount);
    return 1;
  }
  if (crop_percent < 50 || crop_percent > 100) {
    fprintf(stderr, "[record] HDMI_REC_CROP_PERCENT must be 50..100\n");
    return 2;
  }

  time_t now = time(NULL);
  time_t initial_end = now + segment_sec;
  struct tm tmv;
  struct tm end_tmv;
  localtime_r(&now, &tmv);
  localtime_r(&initial_end, &end_tmv);
  char date_dir[32], time_name[32];
  char end_time_name[32];
  strftime(date_dir, sizeof(date_dir), "%Y-%m-%d", &tmv);
  strftime(time_name, sizeof(time_name), "%H-%M-%S", &tmv);
  strftime(end_time_name, sizeof(end_time_name), "%H-%M-%S", &end_tmv);

  char dir0[512], dir1[512];
  snprintf(dir0, sizeof(dir0), "%s/cam0/%s", root, date_dir);
  snprintf(dir1, sizeof(dir1), "%s/cam1/%s", root, date_dir);
  if (mkdir_p(dir0) != 0 || mkdir_p(dir1) != 0) {
    fprintf(stderr, "[record] cannot create output dirs under %s\n", root);
    return 1;
  }
  PathPattern pattern0;
  PathPattern pattern1;
  memset(&pattern0, 0, sizeof(pattern0));
  memset(&pattern1, 0, sizeof(pattern1));
  snprintf(pattern0.root, sizeof(pattern0.root), "%s", root);
  snprintf(pattern1.root, sizeof(pattern1.root), "%s", root);
  pattern0.cam = 0;
  pattern1.cam = 1;
  pattern0.segment_sec = segment_sec;
  pattern1.segment_sec = segment_sec;
  pattern0.base_time = now;
  pattern1.base_time = now;

  int margin_x = width * (100 - crop_percent) / 200;
  int margin_y = height * (100 - crop_percent) / 200;
  long long segment_ns = (long long)segment_sec * 1000000000LL;

  char desc[8192];
  snprintf(desc, sizeof(desc),
    "input-selector name=sel ! queue leaky=downstream max-size-buffers=2 ! waylandsink fullscreen=true sync=false qos=true "
    "v4l2src device=/dev/video0 io-mode=mmap do-timestamp=true ! video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 "
    "! videocrop left=%d right=%d top=%d bottom=%d ! videoscale ! videorate drop-only=true "
    "! video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 ! tee name=t0 "
    "t0. ! queue leaky=downstream max-size-buffers=2 ! sel.sink_0 "
    "t0. ! queue max-size-buffers=10 max-size-time=0 max-size-bytes=0 "
    "! mpph264enc bps=%d bps-min=%d bps-max=%d rc-mode=cbr profile=baseline max-pending=1 gop=%d header-mode=1 "
    "! h264parse config-interval=1 ! splitmuxsink name=mux0 muxer-factory=mp4mux async-finalize=true max-size-time=%lld max-files=%d send-keyframe-requests=true "
    "v4l2src device=/dev/video1 io-mode=mmap do-timestamp=true ! video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 "
    "! videocrop left=%d right=%d top=%d bottom=%d ! videoscale ! videorate drop-only=true "
    "! video/x-raw,format=NV12,width=%d,height=%d,framerate=%d/1 ! tee name=t1 "
    "t1. ! queue leaky=downstream max-size-buffers=2 ! sel.sink_1 "
    "t1. ! queue max-size-buffers=10 max-size-time=0 max-size-bytes=0 "
    "! mpph264enc bps=%d bps-min=%d bps-max=%d rc-mode=cbr profile=baseline max-pending=1 gop=%d header-mode=1 "
    "! h264parse config-interval=1 ! splitmuxsink name=mux1 muxer-factory=mp4mux async-finalize=true max-size-time=%lld max-files=%d send-keyframe-requests=true",
    width, height, fps, margin_x, margin_x, margin_y, margin_y, width, height, fps,
    bitrate, bitrate, bitrate, fps, segment_ns, max_files,
    width, height, fps, margin_x, margin_x, margin_y, margin_y, width, height, fps,
    bitrate, bitrate, bitrate, fps, segment_ns, max_files);

  GError *err = NULL;
  App app;
  memset(&app, 0, sizeof(app));
  app.pipeline = gst_parse_launch(desc, &err);
  if (!app.pipeline) {
    fprintf(stderr, "[gst] parse failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return 1;
  }

  app.selector = gst_bin_get_by_name(GST_BIN(app.pipeline), "sel");
  GstElement *mux0 = gst_bin_get_by_name(GST_BIN(app.pipeline), "mux0");
  GstElement *mux1 = gst_bin_get_by_name(GST_BIN(app.pipeline), "mux1");
  app.pad0 = gst_element_get_static_pad(app.selector, "sink_0");
  app.pad1 = gst_element_get_static_pad(app.selector, "sink_1");
  g_signal_connect(mux0, "format-location", G_CALLBACK(on_format_location), &pattern0);
  g_signal_connect(mux1, "format-location", G_CALLBACK(on_format_location), &pattern1);
  snprintf(app.latest0, sizeof(app.latest0), "%s/%s_%s.mp4", dir0, time_name, end_time_name);
  snprintf(app.latest1, sizeof(app.latest1), "%s/%s_%s.mp4", dir1, time_name, end_time_name);

  app.loop = g_main_loop_new(NULL, FALSE);
  GstBus *bus = gst_element_get_bus(app.pipeline);
  gst_bus_add_watch(bus, on_bus, &app);
  gst_object_unref(bus);

  setup_stdin(&app);
  GIOChannel *stdin_ch = g_io_channel_unix_new(STDIN_FILENO);
  g_io_add_watch(stdin_ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_io, &app);
  g_io_channel_unref(stdin_ch);

  open_event_devices(&app, event_dev);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  g_timeout_add(200, check_signal, &app);

  fprintf(stderr, "[record] timezone: %s\n", getenv("TZ") ? getenv("TZ") : "");
  fprintf(stderr, "[record] cam0 -> %s/cam0/%s/<start>_<end>.mp4\n", pattern0.root, date_dir);
  fprintf(stderr, "[record] cam1 -> %s/cam1/%s/<start>_<end>.mp4\n", pattern1.root, date_dir);
  fprintf(stderr, "[record] HDMI keys: 1=cam0 2=cam1 3=latest paths q=quit\n");

  switch_to(&app, 0);
  gst_element_set_state(app.pipeline, GST_STATE_PLAYING);
  g_main_loop_run(app.loop);

  gst_element_set_state(app.pipeline, GST_STATE_NULL);
  restore_stdin(&app);
  for (int i = 0; i < app.evfd_count; ++i) close(app.evfds[i]);
  if (app.pad0) gst_object_unref(app.pad0);
  if (app.pad1) gst_object_unref(app.pad1);
  if (app.selector) gst_object_unref(app.selector);
  if (mux0) gst_object_unref(mux0);
  if (mux1) gst_object_unref(mux1);
  if (app.pipeline) gst_object_unref(app.pipeline);
  if (app.loop) g_main_loop_unref(app.loop);
  return 0;
}
