/* SPDX-License-Identifier: MIT */
/*
 * audiaki-gui - the desktop recorder.
 *
 * The window owns nothing but the drawing. Audio lives on the engine's capture
 * thread from the moment the device opens, which is why the spectrum moves and
 * the meter reads before you have pressed anything: setting an input level is
 * the first thing you do, and it should not require starting a take you are
 * only going to throw away.
 *
 * This file is the run loop, the engine's lifecycle and the transport actions
 * the buttons and keys stand for. app.h has the state all four files share and
 * says which of them does what.
 */
#include "gui/app.h"

#include "gui/engine.h"
#include "gui/render.h"
#include "gui/ui.h"
#include "gui/viz.h"

#include "audio/format.h"
#include "backend/backend.h"
#include "backend/device.h"
#include "take/take.h"
#include "util/log.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Open the device and build the display for whatever it negotiated. */
static int app_open_engine(app *a)
{
  a->fatal[0] = '\0';

  if (a->device_selected < 0 || a->device_selected >= a->devices.count)
  {
    a->device_selected = 0;
  }

  /* a copy, because the list it came from is rebuilt as hardware comes and goes */
  snprintf(a->active_device, sizeof(a->active_device), "%s",
           a->devices.name[a->device_selected]);
  a->cfg.device = a->active_device;

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
void app_switch_device(app *a, int previous)
{
  int monitoring = aud_engine_monitor_wanted(a->engine);

  app_close_engine(a);

  if (app_open_engine(a) == 0)
  {
    aud_engine_set_monitor(a->engine, monitoring);
    return;
  }

  aud_warn("falling back to '%s'", a->devices.name[previous]);
  a->device_selected = previous;

  if (app_open_engine(a) == 0)
  {
    aud_engine_set_monitor(a->engine, monitoring);
    return;
  }

  /*
   * Both are gone - the one just picked and the one that was working, which
   * on a laptop is one unplugged cable. The window falls back to the "no
   * device" screen and keeps watching; whichever comes back reopens there.
   */
  a->start_monitor = monitoring;
}

/*
 * Open the selected device again once it is back, after the window came up
 * without it or its stream died with the cable. A dead stream cannot be
 * revived, and re-picking the device in the dropdown does not help either -
 * clicking the row that is already selected changes nothing - so a device that
 * returns has to be picked up here or not at all.
 *
 * Only when ALSA reports it again, and only off the back of a list that
 * actually changed: a device that is there but held by another program fails
 * to open every time it is tried, and trying it on every sweep would fill the
 * terminal with the same failure twice a second.
 */
static void app_recover_engine(app *a)
{
  if (a->devices.absent == a->device_selected)
  {
    return;
  }

  if (a->engine != NULL)
  {
    aud_engine_status st;

    aud_engine_status_get(a->engine, &st);
    if (st.state != AUD_ENGINE_FAILED)
    {
      return;
    }

    /* the take, if there was one, was already salvaged when the stream died */
    a->start_monitor = aud_engine_monitor_wanted(a->engine);
    app_close_engine(a);
  }

  if (app_open_engine(a) == 0)
  {
    aud_info("'%s' is back", a->active_device);
    aud_engine_set_monitor(a->engine, a->start_monitor);
  }
}

static void app_pump_audio(app *a)
{
  size_t got;

  while ((got = aud_engine_read_visual(a->engine, a->drain, APP_DRAIN)) > 0)
  {
    aud_viz_push(a->viz, a->drain, got);
    if (got < APP_DRAIN)
    {
      break;
    }
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
  {
    a->peak_hold = peak;
  }
  if (a->peak_hold < 0.0f)
  {
    a->peak_hold = 0.0f;
  }
}

/*
 * Pick the next free take name and start writing to it. Numbering rather than
 * prompting: pressing record should never be the moment you find out you are
 * about to overwrite yesterday's take.
 */
void app_begin_take(app *a)
{
  char path[AUD_ENGINE_PATH_MAX];

  if (aud_take_next(path, sizeof(path), a->prefix) != 0)
  {
    return;
  }

  a->render_note[0] = '\0';
  aud_engine_start(a->engine, path, 0);
}

/*
 * Stop the take, and start rendering its video if that was asked for. The WAV
 * has to be closed first: ffmpeg opens it to read the audio, and a header that
 * has not been patched yet describes a file of zero length.
 */
void app_stop_take(app *a, const aud_engine_status *st)
{
  aud_render_options opts;
  char video[AUD_RENDER_PATH_MAX];
  char take[AUD_ENGINE_PATH_MAX];

  snprintf(take, sizeof(take), "%s", st->path);

  if (aud_engine_stop(a->engine) != 0)
  {
    return;
  } /* the failure is already in the status line */

  a->render_note[0] = '\0';

  if (!a->want_video || take[0] == '\0' || a->render != NULL)
  {
    return;
  }

  if (aud_take_with_extension(video, sizeof(video), take, ".mp4") != 0)
  {
    snprintf(a->render_note, sizeof(a->render_note),
             "cannot work out a video name for that take");
    return;
  }

  aud_render_defaults(&opts);
  opts.wav_path = take;
  opts.video_path = video;
  opts.mode = (aud_viz_mode)a->style_selected;
  opts.width = a->video_width;
  opts.height = a->video_height;
  opts.fps = a->video_fps;
  opts.silent = !a->want_video_audio;

  a->render = aud_render_start(&opts);
  if (a->render == NULL)
  {
    snprintf(a->render_note, sizeof(a->render_note),
             "could not start the video render - is ffmpeg installed?");
  }
}

static void app_pump_render(app *a)
{
  int state;

  if (a->render == NULL)
  {
    return;
  }

  state = aud_render_step(a->render, APP_RENDER_BUDGET);
  if (state == 0)
  {
    return;
  }

  if (state < 0)
  {
    aud_render_finish(a->render, 1);
    snprintf(a->render_note, sizeof(a->render_note), "the video render failed");
  }
  else
  {
    char name[AUD_RENDER_PATH_MAX];

    snprintf(name, sizeof(name), "%s", aud_render_output(a->render));
    if (aud_render_finish(a->render, 0) == 0)
    {
      snprintf(a->render_note, sizeof(a->render_note), "wrote %.200s", name);
    }
    else
    {
      snprintf(a->render_note, sizeof(a->render_note), "the video render failed");
    }
  }

  a->render = NULL;
}

void app_cancel_render(app *a)
{
  if (a->render == NULL)
  {
    return;
  }

  aud_render_finish(a->render, 1);
  a->render = NULL;
  snprintf(a->render_note, sizeof(a->render_note), "video render cancelled");
}

void app_toggle_record(app *a, const aud_engine_status *st)
{
  switch (st->state)
  {
  case AUD_ENGINE_IDLE:
    if (a->render == NULL) /* the renderer has the drawing thread */
    {
      app_begin_take(a);
    }
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

/*
 * The title bar, which is all a window behind another one gets to say. "Is it
 * still recording?" should not need the window raised to answer.
 */
static void app_update_title(app *a, const aud_engine_status *st)
{
  char want[160];

  if (a->render != NULL)
  {
    snprintf(want, sizeof(want), AUDIAKI_NAME " - rendering %.0f%%",
             aud_render_progress(a->render) * 100.0);
  }
  else if (st == NULL || st->state == AUD_ENGINE_FAILED)
  {
    snprintf(want, sizeof(want), AUDIAKI_NAME " - no capture device");
  }
  else if (st->state == AUD_ENGINE_RECORDING || st->state == AUD_ENGINE_PAUSED)
  {
    const char *slash = strrchr(st->path, '/');
    const char *name = slash != NULL ? slash + 1 : st->path;
    unsigned secs = st->elapsed > 0.0 ? (unsigned)st->elapsed : 0u;

    /* whole seconds, not the status line's tenths: this is a window property,
     * and the display server does not need ten of them a second */
    snprintf(want, sizeof(want), AUDIAKI_NAME " - %s %02u:%02u - %.80s",
             st->state == AUD_ENGINE_PAUSED ? "paused" : "recording", secs / 60u,
             secs % 60u, name);
  }
  else
  {
    snprintf(want, sizeof(want), AUDIAKI_NAME);
  }

  if (strcmp(want, a->title) != 0)
  {
    snprintf(a->title, sizeof(a->title), "%s", want);
    SetWindowTitle(a->title);
  }
}

static void handle_keys(app *a, const aud_engine_status *st)
{
  /* the menu owns the keyboard while it is open, same as it owns the mouse */
  if (a->device_menu_open)
  {
    if (IsKeyPressed(KEY_ESCAPE))
    {
      a->device_menu_open = 0;
    }
    return;
  }

  if (IsKeyPressed(KEY_F1) || IsKeyPressed(KEY_SLASH) || IsKeyPressed(KEY_H))
  {
    a->help_open = !a->help_open;
    return;
  }

  /* the shortcut list sits over the window, so nothing behind it answers */
  if (a->help_open)
  {
    if (IsKeyPressed(KEY_ESCAPE))
    {
      a->help_open = 0;
    }
    return;
  }

  if (IsKeyPressed(KEY_SPACE))
  {
    app_toggle_record(a, st);
  }

  if (IsKeyPressed(KEY_S))
  {
    if (a->render != NULL)
    {
      app_cancel_render(a);
    }
    else if (st->state != AUD_ENGINE_IDLE)
    {
      app_stop_take(a, st);
    }
  }

  if (IsKeyPressed(KEY_M))
  {
    aud_engine_set_monitor(a->engine, !aud_engine_monitor_wanted(a->engine));
  }

  if (IsKeyPressed(KEY_V))
  {
    a->style_selected = (int)aud_viz_cycle_mode(a->viz);
  }

  if (IsKeyPressed(KEY_F))
  {
    ToggleFullscreen();
  }

  /* straight to a style, for the one you keep coming back to */
  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    if (IsKeyPressed(KEY_ONE + i))
    {
      a->style_selected = i;
      aud_viz_set_mode(a->viz, (aud_viz_mode)i);
    }
  }
}

int main(int argc, char *argv[])
{
  static app a;
  int rc;

  aud_engine_config_defaults(&a.cfg);
  snprintf(a.prefix, sizeof(a.prefix), "%s", APP_DEFAULT_PREFIX);
  a.monitor_gain = 1.0f;
  a.want_video_audio = 1; /* before parse_args, which only ever clears it */
  a.video_width = AUD_RENDER_DEFAULT_WIDTH;
  a.video_height = AUD_RENDER_DEFAULT_HEIGHT;
  a.video_fps = AUD_RENDER_DEFAULT_FPS;

  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    a.style_labels[i] = aud_viz_mode_name((aud_viz_mode)i);
  }

  {
    /* the same variable the CLI honours; --backend on the command line wins */
    const char *env_backend = getenv("AUDIAKI_BACKEND");

    a.backend = AUD_BACKEND_AUTO;
    if (env_backend != NULL && *env_backend != '\0' &&
        aud_backend_parse(env_backend, &a.backend) != 0)
    {
      aud_warn("ignoring $AUDIAKI_BACKEND=%s: expected auto, pipewire or alsa",
               env_backend);
      a.backend = AUD_BACKEND_AUTO;
    }
  }

  rc = app_parse_args(&a, argc, argv);
  if (rc != 0)
  {
    return rc < 0 ? EXIT_SUCCESS : rc;
  }

  /* before the first enumeration: the dropdown is filled from whichever answers */
  if (aud_backend_select(a.backend) != 0)
  {
    return EXIT_FAILURE;
  }

  /* raylib is chatty on stdout by default; audiaki reports through log.h */
  SetTraceLogLevel(aud_log_get_level() == AUD_LOG_VERBOSE ? LOG_INFO : LOG_WARNING);
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(APP_WIDTH, APP_HEIGHT, AUDIAKI_NAME);
  SetWindowMinSize(APP_MIN_WIDTH, APP_MIN_HEIGHT);
  SetTargetFPS(60);
  SetExitKey(KEY_NULL); /* Escape closing an open take would be unforgivable */

  /* the device -D named, or "default", so the list comes up on the right row */
  snprintf(a.active_device, sizeof(a.active_device), "%s",
           a.cfg.device != NULL ? a.cfg.device : AUD_DEFAULT_DEVICE);

  a.watch = aud_device_watch_create();
  app_load_devices(&a);

  /*
   * A device that will not open is not fatal any more: the window comes up on
   * the "no device" screen and opens whatever is chosen or plugged in later.
   * Exiting to a terminal the user may not have open looks broken, and having
   * to restart the app to see an interface is the same complaint twice.
   */
  if (app_open_engine(&a) == 0)
  {
    /* off unless asked for on the command line: a mic through speakers howls */
    aud_engine_set_monitor(a.engine, a.start_monitor);
  }

  while (!WindowShouldClose())
  {
    aud_engine_status st;

    if (aud_device_watch_changed(a.watch) && app_refresh_devices(&a))
    {
      app_recover_engine(&a);
    }

    if (a.engine == NULL)
    {
      /* still pump the encoder: a video outlives the take it was made from */
      app_pump_render(&a);

      SetMouseCursor(MOUSE_CURSOR_DEFAULT);
      if (IsKeyPressed(KEY_ESCAPE))
      {
        a.device_menu_open = 0;
      }

      /* the shortcut list is not drawn over this screen, so it cannot be left
       * open to reappear when the device comes back */
      a.help_open = 0;
      app_update_title(&a, NULL);

      BeginDrawing();
      app_draw_fatal(&a);
      EndDrawing();
      continue;
    }

    aud_engine_status_get(a.engine, &st);

    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    handle_keys(&a, &st);
    app_pump_audio(&a);
    app_track_peak(&a, (float)st.peak, GetFrameTime());

    /*
     * Before BeginDrawing: the renderer has its own render target to bind, and
     * doing that inside the window's pass would leave the wrong one current.
     */
    app_pump_render(&a);
    app_update_title(&a, &st);

    BeginDrawing();
    app_draw_frame(&a, &st);
    EndDrawing();
  }

  /*
   * A half-written video is not worth waiting for on the way out, but a
   * half-written take is: closing the engine patches the WAV header.
   */
  app_cancel_render(&a);
  app_close_engine(&a);
  aud_device_watch_destroy(a.watch);
  CloseWindow();
  return EXIT_SUCCESS;
}
