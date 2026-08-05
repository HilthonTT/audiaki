/* SPDX-License-Identifier: MIT */
/*
 * audiaki-gui - the desktop recorder.
 *
 * The window owns nothing but the drawing. Audio lives on the engine's capture
 * thread from the moment the device opens, which is why the spectrum moves and
 * the meter reads before you have pressed anything: setting an input level is
 * the first thing you do, and it should not require starting a take you are
 * only going to throw away.
 */
#include "engine.h"
#include "ui.h"
#include "viz.h"

#include "device.h"
#include "format.h"
#include "log.h"
#include "take.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_WIDTH 1100
#define APP_HEIGHT 680
#define APP_MIN_WIDTH 760
#define APP_MIN_HEIGHT 460

#define APP_PAD 18.0f
#define APP_HEADER_H 44.0f
#define APP_TRANSPORT_H 56.0f
#define APP_STATUS_H 34.0f

/* Samples pulled from the engine per drawn frame. */
#define APP_DRAIN 4096u

/* Seconds the peak marker sits at a new maximum before it starts falling. */
#define APP_PEAK_HOLD 1.2f
#define APP_PEAK_FALL 0.55f /* units of full scale per second, once falling */

#define APP_DEFAULT_PREFIX "take"

/*
 * Hardware devices found by ALSA, plus the "default" entry the app prepends.
 * More than this many capture interfaces on one machine is a studio, not a
 * desk, and it needs a device list rather than a dropdown.
 */
#define APP_MAX_DEVICES 33

/*
 * Enough for "card: description" with both fields at their full width, so the
 * label is never the thing that truncates a device name - the dropdown
 * ellipsises to fit its own width, which is the only limit worth having.
 */
#define APP_DEVICE_LABEL 176

typedef struct
{
  aud_engine *engine;
  aud_viz *viz;
  aud_engine_config cfg;

  char prefix[512];
  int start_monitor; /* -M: come up already monitoring */

  /*
   * The device list, built once at startup. Rebuilding it on every open would
   * pick up hot-plugged interfaces, but it also means walking every card in
   * the frame that the menu is clicked, and a recorder is not a device manager.
   */
  char device_name[APP_MAX_DEVICES][64];
  char device_label[APP_MAX_DEVICES][APP_DEVICE_LABEL];
  const char *device_labels[APP_MAX_DEVICES]; /* what the dropdown reads */
  int device_count;
  int device_selected;
  int device_menu_open;

  /* the visualiser style selector, mirroring the mode held by aud_viz */
  const char *style_labels[AUD_VIZ_MODE_COUNT];
  int style_selected;

  float peak_hold;
  float peak_hold_left; /* seconds the marker still has before it decays */
  float monitor_gain;

  /* the reason the engine could not be created, if it could not be */
  char fatal[AUD_ENGINE_ERROR_MAX];

  float drain[APP_DRAIN];
} app;

static void usage(FILE *out, const app *a)
{
  fprintf(out,
          "usage: " AUDIAKI_NAME "-gui [options]\n"
          "\n"
          "  -D, --device NAME    ALSA capture device (default: %s)\n"
          "  -r, --rate HZ        sample rate (default: %u)\n"
          "  -c, --channels N     channel count (default: %u)\n"
          "  -o, --take PREFIX    take name prefix (default: %s)\n"
          "  -s, --style NAME     visualiser style (default: %s)\n"
          "  -M, --monitor        start with playback monitoring on\n"
          "  -v, --verbose        log device negotiation to the terminal\n"
          "  -h, --help           show this and exit\n"
          "\n"
          "styles: bars, mirror, radial, scope, waterfall\n"
          "\n"
          "Takes are numbered from the prefix, so recording never overwrites\n"
          "an existing file and there is no --force to get wrong.\n"
          "\n"
          "keys: space record or pause, S stop, M monitor, V style, F fullscreen\n",
          a->cfg.device, a->cfg.rate, a->cfg.channels, a->prefix,
          aud_viz_mode_name((aud_viz_mode)a->style_selected));
}

/* Returns 0 to carry on, or a process exit code to stop with. */
static int parse_args(app *a, int argc, char **argv)
{
  for (int i = 1; i < argc; i++)
  {
    const char *arg = argv[i];
    const char *value = NULL;

    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
    {
      usage(stdout, a);
      return -1;
    }
    if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
    {
      aud_log_set_level(AUD_LOG_VERBOSE);
      continue;
    }
    if (strcmp(arg, "-M") == 0 || strcmp(arg, "--monitor") == 0)
    {
      a->start_monitor = 1;
      continue;
    }

    /* everything below takes a value */
    if (i + 1 >= argc)
    {
      aud_error("%s needs a value", arg);
      return 2;
    }
    value = argv[++i];

    if (strcmp(arg, "-D") == 0 || strcmp(arg, "--device") == 0)
      a->cfg.device = value;
    else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--rate") == 0)
      a->cfg.rate = (unsigned)strtoul(value, NULL, 10);
    else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--channels") == 0)
      a->cfg.channels = (unsigned)strtoul(value, NULL, 10);
    else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--take") == 0)
      snprintf(a->prefix, sizeof(a->prefix), "%s", value);
    else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--style") == 0)
    {
      aud_viz_mode mode;

      if (aud_viz_mode_from_name(value, &mode) != 0)
      {
        aud_error("unknown style '%s'", value);
        aud_info("styles: bars, mirror, radial, scope, waterfall");
        return 2;
      }
      a->style_selected = (int)mode;
    }
    else
    {
      aud_error("unknown option '%s'", arg);
      usage(stderr, a);
      return 2;
    }
  }

  if (a->cfg.rate == 0 || a->cfg.channels == 0)
  {
    aud_error("rate and channels must be greater than zero");
    return 2;
  }
  return 0;
}

/*
 * Build the list the device dropdown offers: "default" first, because it is
 * what works without knowing anything, then every capture PCM ALSA found.
 * A failed enumeration is not fatal - "default" alone is still a usable app.
 */
static void app_load_devices(app *a)
{
  aud_device_entry *found = NULL;
  int count;

  snprintf(a->device_name[0], sizeof(a->device_name[0]), "%s", AUD_DEFAULT_DEVICE);
  snprintf(a->device_label[0], sizeof(a->device_label[0]), "default (system)");
  a->device_count = 1;

  count = aud_device_enumerate(&found);
  for (int i = 0; i < count && a->device_count < APP_MAX_DEVICES; i++)
  {
    int slot = a->device_count;

    snprintf(a->device_name[slot], sizeof(a->device_name[slot]), "%s", found[i].name);
    if (found[i].description[0] != '\0')
      snprintf(a->device_label[slot], sizeof(a->device_label[slot]), "%s: %s",
               found[i].card, found[i].description);
    else
      snprintf(a->device_label[slot], sizeof(a->device_label[slot]), "%s", found[i].card);
    a->device_count++;
  }
  free(found);

  if (count > 0 && a->device_count == APP_MAX_DEVICES)
    aud_warn("more than %d capture devices; the rest are not offered in the window",
             APP_MAX_DEVICES - 1);

  for (int i = 0; i < a->device_count; i++)
    a->device_labels[i] = a->device_label[i];

  /*
   * Point the selection at whatever -D asked for, so the dropdown opens
   * showing the device actually in use rather than the top of the list.
   */
  a->device_selected = 0;
  for (int i = 0; i < a->device_count; i++)
  {
    if (a->cfg.device != NULL && strcmp(a->cfg.device, a->device_name[i]) == 0)
    {
      a->device_selected = i;
      return;
    }
  }

  /* a -D naming something not in the list still has to appear in it */
  if (a->cfg.device != NULL && strcmp(a->cfg.device, AUD_DEFAULT_DEVICE) != 0 &&
      a->device_count < APP_MAX_DEVICES)
  {
    int slot = a->device_count;

    snprintf(a->device_name[slot], sizeof(a->device_name[slot]), "%s", a->cfg.device);
    snprintf(a->device_label[slot], sizeof(a->device_label[slot]), "%s", a->cfg.device);
    a->device_labels[slot] = a->device_label[slot];
    a->device_selected = slot;
    a->device_count++;
  }
}

/* Open the device and build the display for whatever it negotiated. */
static int app_open_engine(app *a)
{
  a->fatal[0] = '\0';
  a->cfg.device = a->device_name[a->device_selected];

  a->engine = aud_engine_create(&a->cfg);
  if (a->engine == NULL)
  {
    snprintf(a->fatal, sizeof(a->fatal), "cannot open capture device '%s'",
             a->cfg.device);
    return -1;
  }

  a->viz = aud_viz_create(aud_engine_rate(a->engine), AUD_VIZ_DEFAULT_BANDS);
  if (a->viz == NULL)
  {
    snprintf(a->fatal, sizeof(a->fatal), "cannot set up the spectrum display");
    aud_engine_destroy(a->engine);
    a->engine = NULL;
    return -1;
  }

  /* the style survives a device change; the analyser behind it does not */
  aud_viz_set_mode(a->viz, (aud_viz_mode)a->style_selected);
  aud_engine_set_monitor_gain(a->engine, a->monitor_gain);
  return 0;
}

static void app_close_engine(app *a)
{
  aud_viz_destroy(a->viz);
  a->viz = NULL;
  aud_engine_destroy(a->engine);
  a->engine = NULL;
}

/*
 * Swap capture devices. There is no way to re-point a running ALSA stream, and
 * the new device may negotiate a different rate that the analyser has to be
 * rebuilt around, so this tears the engine down and stands a new one up.
 *
 * The caller only offers this when no take is open, so nothing can be lost
 * here. If the new device will not open, fall back to the previous one rather
 * than leaving the window with no audio at all.
 */
static void app_switch_device(app *a, int previous)
{
  int monitoring = aud_engine_monitor_wanted(a->engine);

  app_close_engine(a);

  if (app_open_engine(a) == 0)
  {
    aud_engine_set_monitor(a->engine, monitoring);
    return;
  }

  aud_warn("falling back to '%s'", a->device_name[previous]);
  a->device_selected = previous;

  if (app_open_engine(a) == 0)
    aud_engine_set_monitor(a->engine, monitoring);
}

/* Move everything the capture thread has produced into the analyser. */
static void app_pump_audio(app *a)
{
  size_t got;

  while ((got = aud_engine_read_visual(a->engine, a->drain, APP_DRAIN)) > 0)
  {
    aud_viz_push(a->viz, a->drain, got);
    if (got < APP_DRAIN)
      break;
  }

  aud_viz_update(a->viz, GetFrameTime());
}

/*
 * A peak marker that holds at a new maximum and then slides down, because a
 * bare instantaneous reading moves too fast to catch the transient that
 * actually clipped.
 */
static void app_track_peak(app *a, float peak, float dt)
{
  if (peak >= a->peak_hold)
  {
    a->peak_hold = peak;
    a->peak_hold_left = APP_PEAK_HOLD;
    return;
  }

  if (a->peak_hold_left > 0.0f)
  {
    a->peak_hold_left -= dt;
    return;
  }

  a->peak_hold -= APP_PEAK_FALL * dt;
  if (a->peak_hold < peak)
    a->peak_hold = peak;
  if (a->peak_hold < 0.0f)
    a->peak_hold = 0.0f;
}

/*
 * Pick the next free take name and start writing to it. Numbering rather than
 * prompting: pressing record should never be the moment you find out you are
 * about to overwrite yesterday's take.
 */
static void app_begin_take(app *a)
{
  char path[AUD_ENGINE_PATH_MAX];

  if (aud_take_next(path, sizeof(path), a->prefix) != 0)
    return;

  aud_engine_start(a->engine, path, 0);
}

static void app_toggle_record(app *a, const aud_engine_status *st)
{
  switch (st->state)
  {
  case AUD_ENGINE_IDLE:
    app_begin_take(a);
    return;
  case AUD_ENGINE_RECORDING:
    aud_engine_pause(a->engine);
    return;
  case AUD_ENGINE_PAUSED:
    aud_engine_resume(a->engine);
    return;
  case AUD_ENGINE_FAILED:
  default:
    return;
  }
}

/* -- drawing --------------------------------------------------------------- */

/* The device dropdown's slot, which the header leaves clear for it. */
static Rectangle header_picker(Rectangle r)
{
  Rectangle picker;

  picker.width = 260.0f;
  if (picker.width > r.width * 0.4f)
    picker.width = r.width * 0.4f;
  picker.height = 32.0f;
  picker.x = r.x + r.width - picker.width;
  picker.y = r.y + (r.height - picker.height) / 2.0f;
  return picker;
}

static void draw_header(const app *a, Rectangle r)
{
  Rectangle picker = header_picker(r);
  char detail[128];

  aud_ui_text(r.x, r.y + 6.0f, 28, AUD_UI_TEXT, AUDIAKI_NAME);

  /* what the device negotiated, immediately left of the picker that chose it */
  snprintf(detail, sizeof(detail), "%u Hz   %u ch   %s", aud_engine_rate(a->engine),
           aud_engine_channels(a->engine), aud_format_name(aud_engine_format(a->engine)));
  aud_ui_text_right(picker.x - 16.0f, r.y + 14.0f, 18, AUD_UI_MUTED, detail);
}

/*
 * The record light. Pausing freezes it rather than hiding it: the take is
 * still open, and a control that vanishes reads as "stopped".
 */
static void draw_record_light(Rectangle r, aud_engine_state state, double time)
{
  Color c = AUD_UI_MUTED;
  float alpha = 1.0f;

  if (state == AUD_ENGINE_RECORDING)
  {
    c = AUD_UI_RECORD;
    alpha = 0.55f + 0.45f * (float)((sin(time * 5.0) + 1.0) / 2.0);
  }
  else if (state == AUD_ENGINE_PAUSED)
  {
    c = AUD_UI_WARN;
  }

  c.a = (unsigned char)(alpha * 255.0f);
  DrawCircle((int)(r.x + 8.0f), (int)(r.y + r.height / 2.0f), 7.0f, c);
}

static const char *state_label(aud_engine_state state)
{
  switch (state)
  {
  case AUD_ENGINE_RECORDING:
    return "recording";
  case AUD_ENGINE_PAUSED:
    return "paused";
  case AUD_ENGINE_FAILED:
    return "device lost";
  case AUD_ENGINE_IDLE:
  default:
    return "ready";
  }
}

static void draw_transport(app *a, Rectangle r, const aud_engine_status *st)
{
  float bw = 132.0f;
  float gap = 10.0f;
  Rectangle rec = {r.x, r.y, bw, r.height};
  Rectangle pause = {r.x + bw + gap, r.y, bw, r.height};
  Rectangle stop = {r.x + 2.0f * (bw + gap), r.y, bw, r.height};
  int recording = st->state == AUD_ENGINE_RECORDING;
  int paused = st->state == AUD_ENGINE_PAUSED;
  /* an open menu covers these, so nothing under it should take a click */
  int live = (recording || paused) && !a->device_menu_open;
  int usable = st->state != AUD_ENGINE_FAILED && !a->device_menu_open;

  if (aud_ui_button(rec, live ? "Recording" : "Record", AUD_UI_RECORD, usable && !live))
    app_begin_take(a);

  if (aud_ui_button(pause, paused ? "Resume" : "Pause", AUD_UI_WARN, live))
  {
    if (paused)
      aud_engine_resume(a->engine);
    else
      aud_engine_pause(a->engine);
  }

  if (aud_ui_button(stop, "Stop", AUD_UI_ACCENT, live))
    aud_engine_stop(a->engine);

  /* monitoring sits at the right hand end, away from the transport */
  {
    float slider_w = 150.0f;
    float toggle_w = 128.0f;
    Rectangle slider = {r.x + r.width - slider_w, r.y + (r.height - 26.0f) / 2.0f,
                        slider_w, 26.0f};
    Rectangle toggle = {slider.x - gap - toggle_w, r.y, toggle_w, r.height};
    int wanted = aud_engine_monitor_wanted(a->engine);

    if (aud_ui_toggle(toggle, st->monitoring ? "Monitor on" : "Monitor", wanted,
                      AUD_UI_OK, usable))
      aud_engine_set_monitor(a->engine, !wanted);

    if (aud_ui_slider(slider, &a->monitor_gain, 0.0f, 2.0f, AUD_UI_OK, usable))
      aud_engine_set_monitor_gain(a->engine, a->monitor_gain);
  }
}

static void draw_status(const app *a, Rectangle r, const aud_engine_status *st)
{
  char clock[16];
  char right[320];
  float meter_w = 260.0f;
  Rectangle meter;
  float x = r.x;

  draw_record_light(r, st->state, GetTime());
  x += 26.0f;

  aud_ui_format_clock(clock, sizeof(clock), st->elapsed);
  aud_ui_text(x, r.y + 6.0f, 24, AUD_UI_TEXT, clock);
  x += (float)MeasureText(clock, 24) + 20.0f;

  aud_ui_text(x, r.y + 10.0f, 18, AUD_UI_MUTED, state_label(st->state));
  x += 110.0f;

  meter.x = x;
  meter.y = r.y + 9.0f;
  meter.width = meter_w;
  meter.height = 16.0f;
  aud_ui_meter(meter, (float)st->peak, a->peak_hold);
  x += meter_w + 14.0f;

  {
    char db[32];

    snprintf(db, sizeof(db), "%.1f dBFS", aud_format_dbfs(st->peak));
    aud_ui_text(x, r.y + 10.0f, 18, st->clipped ? AUD_UI_RECORD : AUD_UI_MUTED, db);
  }

  /*
   * One line on the right for whatever most needs saying: a failure first,
   * then clipping, then the file being written.
   */
  if (st->error[0] != '\0')
  {
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_RECORD, st->error);
    return;
  }

  if (st->clipped)
  {
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_RECORD,
                      "input clipped - lower the level on the device");
    return;
  }

  if (st->path[0] != '\0')
  {
    const char *slash = strrchr(st->path, '/');
    const char *name = slash != NULL ? slash + 1 : st->path;

    /*
     * The precision bounds both the buffer and the layout: a take living at
     * the end of a very long path should not push the size off the window.
     */
    if (st->xruns > 0)
      snprintf(right, sizeof(right), "%.200s   %.1f MiB   %u xrun(s)", name,
               (double)st->bytes / (1024.0 * 1024.0), st->xruns);
    else
      snprintf(right, sizeof(right), "%.200s   %.1f MiB", name,
               (double)st->bytes / (1024.0 * 1024.0));

    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_MUTED, right);
  }
  else
  {
    snprintf(right, sizeof(right), "next take: %.200s-...", a->prefix);
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_MUTED, right);
  }
}

/* The window when there is no device to draw from. */
static void draw_fatal(const app *a)
{
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle line = screen;

  ClearBackground(AUD_UI_BG);

  line.height = 40.0f;
  line.y = screen.height / 2.0f - 70.0f;
  aud_ui_text_centred(line, 28, AUD_UI_RECORD, a->fatal);

  line.y += 46.0f;
  aud_ui_text_centred(line, 18, AUD_UI_MUTED,
                      "run '" AUDIAKI_NAME " --list' to see the capture devices, then "
                      "pass one with -D");

  line.y += 30.0f;
  aud_ui_text_centred(line, 18, AUD_UI_MUTED,
                      "the device may also be held by another program");
}

static void draw_frame(app *a, const aud_engine_status *st)
{
  float w = (float)GetScreenWidth();
  float h = (float)GetScreenHeight();
  Rectangle header = {APP_PAD, APP_PAD, w - 2.0f * APP_PAD, APP_HEADER_H};
  Rectangle stage;
  Rectangle transport;
  Rectangle status;
  int live = st->state == AUD_ENGINE_RECORDING || st->state == AUD_ENGINE_PAUSED;
  int previous = a->device_selected;

  ClearBackground(AUD_UI_BG);

  status.x = APP_PAD;
  status.width = w - 2.0f * APP_PAD;
  status.height = APP_STATUS_H;
  status.y = h - APP_PAD - APP_STATUS_H;

  transport.x = APP_PAD;
  transport.width = status.width;
  transport.height = APP_TRANSPORT_H;
  transport.y = status.y - 10.0f - APP_TRANSPORT_H;

  stage.x = APP_PAD;
  stage.width = status.width;
  stage.y = header.y + header.height + 8.0f;
  stage.height = transport.y - 16.0f - stage.y;

  draw_header(a, header);

  if (stage.height > 40.0f)
  {
    Rectangle inner = {stage.x + 12.0f, stage.y + 12.0f, stage.width - 24.0f,
                       stage.height - 24.0f};

    /* the stage is drawn nearly black so the additive glow has somewhere to go */
    DrawRectangleRounded(stage, 10.0f / stage.height, 8, BLACK);
    DrawRectangleRoundedLines(stage, 10.0f / stage.height, 8, AUD_UI_EDGE);
    aud_viz_draw(a->viz, inner);

    /*
     * The style selector rides on the stage rather than in the chrome. There
     * is no room for five more labels in the header at the minimum window
     * width, and a control that sits on what it changes is easy to find.
     */
    if (stage.width > 360.0f && stage.height > 90.0f)
    {
      float tabs_w = 92.0f * (float)AUD_VIZ_MODE_COUNT;
      Rectangle tabs;

      if (tabs_w > stage.width - 24.0f)
        tabs_w = stage.width - 24.0f;

      tabs.x = stage.x + stage.width - 12.0f - tabs_w;
      tabs.y = stage.y + 12.0f;
      tabs.width = tabs_w;
      tabs.height = 26.0f;

      if (aud_ui_tabs(tabs, a->style_labels, AUD_VIZ_MODE_COUNT, &a->style_selected,
                      !a->device_menu_open, 1))
        aud_viz_set_mode(a->viz, (aud_viz_mode)a->style_selected);
    }
  }

  draw_transport(a, transport, st);
  draw_status(a, status, st);

  /*
   * Last, because an open menu has to cover the stage and the transport. It is
   * disabled while a take is open: swapping the device means closing the
   * capture stream, and doing that mid-take would truncate the recording.
   */
  if (aud_ui_dropdown(header_picker(header), a->device_labels, a->device_count,
                      &a->device_selected, &a->device_menu_open, !live))
    app_switch_device(a, previous);

  if (live && a->device_menu_open)
    a->device_menu_open = 0;
}

static void handle_keys(app *a, const aud_engine_status *st)
{
  /* the menu owns the keyboard while it is open, same as it owns the mouse */
  if (a->device_menu_open)
  {
    if (IsKeyPressed(KEY_ESCAPE))
      a->device_menu_open = 0;
    return;
  }

  if (IsKeyPressed(KEY_SPACE))
    app_toggle_record(a, st);

  if (IsKeyPressed(KEY_S) && st->state != AUD_ENGINE_IDLE)
    aud_engine_stop(a->engine);

  if (IsKeyPressed(KEY_M))
    aud_engine_set_monitor(a->engine, !aud_engine_monitor_wanted(a->engine));

  if (IsKeyPressed(KEY_V))
    a->style_selected = (int)aud_viz_cycle_mode(a->viz);

  if (IsKeyPressed(KEY_F))
    ToggleFullscreen();
}

int main(int argc, char *argv[])
{
  static app a;
  int rc;

  aud_engine_config_defaults(&a.cfg);
  snprintf(a.prefix, sizeof(a.prefix), "%s", APP_DEFAULT_PREFIX);
  a.monitor_gain = 1.0f;

  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
    a.style_labels[i] = aud_viz_mode_name((aud_viz_mode)i);

  rc = parse_args(&a, argc, argv);
  if (rc != 0)
    return rc < 0 ? EXIT_SUCCESS : rc;

  /* raylib is chatty on stdout by default; audiaki reports through log.h */
  SetTraceLogLevel(aud_log_get_level() == AUD_LOG_VERBOSE ? LOG_INFO : LOG_WARNING);
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(APP_WIDTH, APP_HEIGHT, AUDIAKI_NAME);
  SetWindowMinSize(APP_MIN_WIDTH, APP_MIN_HEIGHT);
  SetTargetFPS(60);
  SetExitKey(KEY_NULL); /* Escape closing an open take would be unforgivable */

  app_load_devices(&a);

  if (app_open_engine(&a) != 0)
  {
    /*
     * Still show a window. A recorder that exits to a terminal the user may
     * not have open is a recorder that looks broken.
     */
    while (!WindowShouldClose())
    {
      BeginDrawing();
      draw_fatal(&a);
      EndDrawing();
    }
    CloseWindow();
    return EXIT_FAILURE;
  }

  /* off unless asked for on the command line: a mic through speakers howls */
  aud_engine_set_monitor(a.engine, a.start_monitor);

  while (!WindowShouldClose())
  {
    aud_engine_status st;

    aud_engine_status_get(a.engine, &st);

    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    handle_keys(&a, &st);
    app_pump_audio(&a);
    app_track_peak(&a, (float)st.peak, GetFrameTime());

    BeginDrawing();
    draw_frame(&a, &st);
    EndDrawing();
  }

  /* finalises the take: the WAV header still needs patching */
  app_close_engine(&a);
  CloseWindow();
  return EXIT_SUCCESS;
}
