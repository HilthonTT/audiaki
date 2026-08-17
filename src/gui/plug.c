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
 * This file is the app's own lifecycle - what happens once at the start, what
 * happens every frame, and what happens on the way out. What each frame is
 * made of lives beside it: take.c records, actions.c carries out whatever was
 * asked for, keys.c decides what the keyboard was asking, and screen.c draws.
 * app.h has the state they share and says which of them does what.
 *
 * It is also where the app meets the shell that runs it: the four entry points
 * at the bottom are the whole of that boundary, and in a development build
 * everything in this directory except main.c and engine.c can be rebuilt and
 * loaded again behind them without the window closing. See hotreload/plug.h.
 */
#include "gui/app.h"

#include "gui/engine.h"
#include "gui/keys.h"
#include "gui/render.h"
#include "gui/ui.h"
#include "gui/viz.h"

#include "hotreload/plug.h"

#include "audio/format.h"
#include "backend/backend.h"
#include "backend/device.h"
#include "edit/edit.h"
#include "edit/project.h"
#include "take/take.h"
#include "util/config.h"
#include "util/log.h"
#include "util/path.h"
#include "version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_set_status(app *a, const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  vsnprintf(a->status, sizeof(a->status), fmt, args);
  va_end(args);
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
  else if (a->project_path[0] != '\0')
  {
    /* the session, and whether it has moved on from what is on disk */
    snprintf(want, sizeof(want), AUDIAKI_NAME " - %.100s%s",
             aud_path_basename(a->project_path), a->project_dirty ? " *" : "");
  }
  else
  {
    snprintf(want, sizeof(want), AUDIAKI_NAME "%s",
             a->project_dirty ? " - unsaved session" : "");
  }

  if (strcmp(want, a->title) != 0)
  {
    snprintf(a->title, sizeof(a->title), "%s", want);
    SetWindowTitle(a->title);
  }
}

/*
 * A frame of the keyboard, carried out.
 *
 * Three steps rather than one: read what is being pressed, work out what it was
 * asking for, do it. The middle step is the one with the judgement in it and is
 * the one that has tests - see keys.h - and this is the whole of what holds the
 * three together.
 */
static void app_handle_keys(app *a, const aud_engine_status *st)
{
  app_input in;
  app_cmd cmds[APP_CMD_MAX];
  int count;

  app_input_read(&in);
  count = app_cmd_map(a, &in, st, cmds, APP_CMD_MAX);

  for (int i = 0; i < count; i++)
  {
    app_cmd_run(a, &cmds[i], st);
  }
}

/*
 * Put the take prefix in the folder takes are kept in, and make sure that
 * folder exists. Once, at startup, so nothing downstream is holding half a
 * path - and early enough that a folder that cannot be created is a message
 * before the window rather than a take that will not start inside it.
 */
static void app_place_prefix(app *a)
{
  char placed[AUD_PATH_MAX];

  if (a->take_dir[0] == '\0' || strchr(a->prefix, '/') != NULL)
  {
    return;
  }

  if (aud_path_mkdirs(a->take_dir) != 0)
  {
    aud_perror("cannot use %s, keeping takes here instead", a->take_dir);
    a->take_dir[0] = '\0';
    return;
  }

  if (aud_path_place(placed, sizeof(placed), a->take_dir, a->prefix) != 0 ||
      (size_t)snprintf(a->prefix, sizeof(a->prefix), "%s", placed) >= sizeof(a->prefix))
  {
    aud_warn("'%s' in '%s' is too long a name, keeping takes here instead", a->prefix,
             a->take_dir);
    snprintf(a->prefix, sizeof(a->prefix), "%s", APP_DEFAULT_PREFIX);
  }
}

/* -- the app, as the shell sees it ------------------------------------------ */

/*
 * The state, on the heap.
 *
 * A static here would be a window that lost its session every time the code
 * behind it was reloaded: a reload unmaps this library's own memory along with
 * the rest of it. What is on the heap outlives that, and the shell hands the
 * pointer to whatever library comes next - see aud_plug_pre_reload() below.
 */
static app *plug;

/* Everything init put up, for the paths that give up before there is a window. */
static void app_free(app *a)
{
  aud_repair_panel_free(&a->repair);
  free(a->take_buf);
  free(a);
}

/*
 * Point the style selector at the visualiser's own names for its styles.
 *
 * Its own, and therefore this library's: they are string literals in viz.c,
 * and a reload frees the pages they live on. Anything the state holds that
 * points into the code has to be found again afterwards, and this is the only
 * such thing there is - see aud_plug_post_reload().
 */
static void app_name_styles(app *a)
{
  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    a->style_labels[i] = aud_viz_mode_name((aud_viz_mode)i);
  }
}

int aud_plug_init(int argc, char **argv)
{
  aud_config cfg;
  app *a;
  int rc;

  a = calloc(1u, sizeof(*a));
  if (a == NULL)
  {
    aud_error("cannot allocate the window's state");
    return EXIT_FAILURE;
  }
  plug = a;
  a->self_size = sizeof(*a);

  aud_engine_config_defaults(&a->cfg);
  snprintf(a->prefix, sizeof(a->prefix), "%s", APP_DEFAULT_PREFIX);
  a->monitor_gain = 1.0f;
  a->input_gain = 1.0f;       /* the samples the interface delivered, untouched */
  a->drawer = APP_DRAWER_VIZ; /* what the window has always come up showing */
  a->viz_height = APP_VIZ_OPEN_H;
  aud_repair_panel_init(&a->repair);
  aud_timeline_init(&a->timeline);
  aud_clipboard_init(&a->clipboard);
  aud_player_init(&a->player);
  aud_preview_init(&a->preview);
  a->record_track = -1;
  a->last_take_track = -1;
  /*
   * On by default: playing along to what is already there is what a second take
   * is for, and a window that needed the feature turning on before it would do
   * the obvious thing would be hiding it. With an empty project it costs
   * nothing, there being nothing to play.
   */
  a->overdub = 1;
  a->click_gain = (float)AUD_CLICK_DEFAULT_GAIN;
  /* the sentinel, until the config file or --latency says otherwise below */
  a->latency_ms = -1.0;

  /*
   * A quarter of a second of drain per pass at the usual channel counts. The
   * ring holds four seconds, so this empties it comfortably faster than it
   * fills even on a frame that took far longer than a frame should. How many
   * frames that is depends on the device, and is worked out in app_open_engine.
   */
  a->take_buf = malloc(APP_TAKE_BUF_SAMPLES * sizeof(float));
  if (a->take_buf == NULL)
  {
    aud_error("cannot allocate the take buffer");
    app_free(a);
    plug = NULL;
    return EXIT_FAILURE;
  }

  /*
   * The same file the CLI reads, and for the same reason: where takes are kept
   * is answered once and then meant every session. Before parse_args, which is
   * what lets --dir and --no-dialog say otherwise.
   */
  aud_config_load(&cfg);
  snprintf(a->take_dir, sizeof(a->take_dir), "%s", cfg.take_dir);
  a->latency_ms = cfg.latency_ms; /* --latency on the command line still wins */
  if (cfg.input_gain >= 0.0)
  {
    a->input_gain = (float)cfg.input_gain; /* and --gain still wins over this */
  }
  /*
   * Off unless the config says otherwise, and deliberately the other way round
   * from the terminal recorder. There, a take is a file and the question is
   * where to keep it. Here it lands on the timeline the moment it stops and is
   * ready to be cut about; a dialog between playing something and editing it
   * would be a dialog in the way. Where the WAV itself goes is take_dir's job,
   * and Export is how a finished mix leaves.
   */
  a->want_dialog = cfg.prompt == AUD_PROMPT_ALWAYS;
  a->want_video_audio = 1; /* before parse_args, which only ever clears it */
  a->video_width = AUD_RENDER_DEFAULT_WIDTH;
  a->video_height = AUD_RENDER_DEFAULT_HEIGHT;
  a->video_fps = AUD_RENDER_DEFAULT_FPS;

  app_name_styles(a);

  {
    /* the same variable the CLI honours; --backend on the command line wins */
    const char *env_backend = getenv("AUDIAKI_BACKEND");

    a->backend = AUD_BACKEND_AUTO;
    if (env_backend != NULL && *env_backend != '\0' &&
        aud_backend_parse(env_backend, &a->backend) != 0)
    {
      aud_warn("ignoring $AUDIAKI_BACKEND=%s: expected auto, pipewire or alsa",
               env_backend);
      a->backend = AUD_BACKEND_AUTO;
    }
  }

  rc = app_parse_args(a, argc, argv);
  if (rc != 0)
  {
    app_free(a);
    plug = NULL;
    return rc < 0 ? -1 : rc;
  }

  app_place_prefix(a);

  /* before the first enumeration: the dropdown is filled from whichever answers */
  if (aud_backend_select(a->backend) != 0)
  {
    app_free(a);
    plug = NULL;
    return EXIT_FAILURE;
  }

  /* raylib is chatty on stdout by default; audiaki reports through log.h */
  SetTraceLogLevel(aud_log_get_level() == AUD_LOG_VERBOSE ? LOG_INFO : LOG_WARNING);
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(APP_WIDTH, APP_HEIGHT, AUDIAKI_NAME);
  SetWindowMinSize(APP_MIN_WIDTH, APP_MIN_HEIGHT);
  SetTargetFPS(60);
  SetExitKey(KEY_NULL); /* Escape closing an open take would be unforgivable */

  /*
   * The project takes its rate from whatever the device negotiated, so a take
   * lands on the timeline at the rate it was recorded at rather than being
   * declared to be at some other one.
   */
  aud_doc_init(&a->doc, a->cfg.rate);

  /* the device -D named, or "default", so the list comes up on the right row */
  snprintf(a->active_device, sizeof(a->active_device), "%s",
           a->cfg.device != NULL ? a->cfg.device : AUD_DEFAULT_DEVICE);

  a->watch = aud_device_watch_create();
  app_load_devices(a);

  /*
   * A device that will not open is not fatal any more: the window comes up on
   * the "no device" screen and opens whatever is chosen or plugged in later.
   * Exiting to a terminal the user may not have open looks broken, and having
   * to restart the app to see an interface is the same complaint twice.
   */
  if (app_open_engine(a) == 0)
  {
    /* off unless asked for on the command line: a mic through speakers howls */
    aud_engine_set_monitor(a->engine, a->start_monitor);
    aud_doc_init(&a->doc, aud_engine_rate(a->engine));
  }

  /* whatever was named on the command line, now that there is a rate to put
   * it at and a window to say so in if one of them will not open */
  for (int i = 0; i < a->open_count; i++)
  {
    /*
     * A session opens as a session and a WAV as a track, so one argument list
     * covers both and 'audiaki-gui yesterday.aki' does what it looks like.
     * Only the first project named: opening a second would throw the first away.
     */
    if (aud_project_is_project(a->open_paths[i]))
    {
      const char *why = NULL;

      if (a->project_path[0] != '\0')
      {
        aud_warn("only one project can be open; ignoring %s", a->open_paths[i]);
      }
      else if (aud_project_load(&a->doc, a->open_paths[i], &why) != 0)
      {
        aud_error("cannot open %s: %s", a->open_paths[i], why != NULL ? why : "unknown");
        app_set_status(a, "cannot open %.80s: %s", aud_path_basename(a->open_paths[i]),
                       why != NULL ? why : "unknown");
      }
      else
      {
        snprintf(a->project_path, sizeof(a->project_path), "%s", a->open_paths[i]);
        a->project_dirty = 0;
      }
      continue;
    }

    app_load_track(a, a->open_paths[i]);
  }

  /*
   * After the files, so a tempo asked for on the command line is the one that
   * holds rather than the one a session happened to be saved at. Said nothing
   * and the project keeps its own, which for a new one is 120 to the bar of
   * four.
   */
  if (a->start_tempo > 0.0 || a->start_beats > 0u)
  {
    aud_doc_set_tempo(&a->doc, a->start_tempo > 0.0 ? a->start_tempo : a->doc.tempo,
                      a->start_beats > 0u ? a->start_beats : a->doc.beats_per_bar);
  }

  if (a->doc.count > 0)
  {
    aud_timeline_fit(&a->timeline, &a->doc,
                     (float)APP_WIDTH - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W);
  }

  return 0;
}

/*
 * The window manager has asked to close. Returns non-zero when it may.
 *
 * A question going up is an answer of "not yet": the close is dropped, the
 * dialog stays, and pressing the button again while it is up changes nothing -
 * see app_confirm_quit(). The answer to it is what actually leaves.
 */
static int closing_now(app *a, int asked)
{
  if (!asked)
  {
    return 0;
  }

  return !app_confirm_quit(a);
}

bool aud_plug_frame(bool close_requested)
{
  app *a = plug;
  aud_engine_status st;
  int quit = 0;

  /*
   * Before the watch, which is what replaces a dead engine with a live one:
   * the take the stream died under has to be closed out on the timeline while
   * the engine that was writing it is still there to be asked about it.
   */
  app_check_capture_loss(a);

  if (aud_device_watch_changed(a->watch) && app_refresh_devices(a))
  {
    app_recover_engine(a);
  }

  if (a->engine == NULL)
  {
    /* still pump the encoder: a video outlives the take it was made from */
    app_pump_render(a);

    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    if (IsKeyPressed(KEY_ESCAPE))
    {
      a->device_menu_open = 0;
    }

    /* the shortcut list is not drawn over this screen, so it cannot be left
     * open to reappear when the device comes back */
    a->help_open = 0;
    app_update_title(a, NULL);

    if (closing_now(a, close_requested))
    {
      return false;
    }

    BeginDrawing();
    quit = app_draw_fatal(a);
    EndDrawing();
    return quit ? false : true;
  }

  aud_engine_status_get(a->engine, &st);

  SetMouseCursor(MOUSE_CURSOR_DEFAULT);
  app_handle_keys(a, &st);
  app_pump_audio(a);
  app_pump_take(a);

  /*
   * Before the drawing, so the playhead the frame shows is where playback
   * actually is rather than where it was when the last frame started.
   */
  if (aud_player_pump(&a->player, &a->doc))
  {
    /* it reached the end by itself; leave the cursor where it started, the
     * way a transport does, so pressing play again repeats the same passage */
    app_set_status(a, "played to the end");
  }

  /*
   * The file the save dialog is auditioning, fed the same way and from the
   * same place. Nothing on the timeline moves for it: it is a file being
   * heard, not a take being played - see preview.h.
   */
  aud_preview_pump(&a->preview);
  if (aud_player_playing(&a->player))
  {
    aud_timeline_reveal(&a->timeline, &a->doc, aud_player_head(&a->player),
                        a->timeline.wave_w);
  }
  /* a take being recorded scrolls into view the same way */
  else if (a->record_track >= 0)
  {
    aud_timeline_reveal(&a->timeline, &a->doc,
                        a->record_at + (uint64_t)(st.elapsed * a->doc.rate),
                        a->timeline.wave_w);
  }
  app_track_peak(a, (float)st.peak, GetFrameTime());

  /*
   * Before BeginDrawing: the renderer has its own render target to bind, and
   * doing that inside the window's pass would leave the wrong one current.
   */
  app_pump_render(a);
  app_update_title(a, &st);

  if (closing_now(a, close_requested))
  {
    return false;
  }

  BeginDrawing();
  quit = app_draw_frame(a, &st);
  EndDrawing();

  return quit ? false : true;
}

void aud_plug_shutdown(void)
{
  app *a = plug;

  /*
   * Edits that were never saved, on the way out.
   *
   * The takes themselves are always safe - each is a closed WAV the moment it
   * stopped - but what was done to them since only exists in memory, and the
   * window closing is not a decision to throw that away. A session with a file
   * is written back to it; one that never had a name gets a recovery file
   * beside the takes, and is said so on the terminal.
   */
  if (a->project_dirty && a->doc.count > 0)
  {
    char recovery[AUD_PATH_MAX];
    const char *where = a->project_path;
    const char *why = NULL;

    if (where[0] == '\0')
    {
      if (aud_path_place(recovery, sizeof(recovery), a->take_dir,
                         "recovered" AUD_PROJECT_EXT) == 0)
      {
        where = recovery;
      }
    }

    if (where[0] != '\0' && aud_project_save(&a->doc, where, &why) == 0)
    {
      aud_info("unsaved edits written to %s", where);
    }
    else if (why != NULL)
    {
      aud_warn("could not write the unsaved edits: %s", why);
    }
  }

  /*
   * A half-written video is not worth waiting for on the way out, but a
   * half-written take is: closing the engine patches the WAV header.
   */
  app_cancel_render(a);
  /* a desktop file chooser is a child process, and it outlives the window that
   * asked for it unless it is taken down with it */
  app_save_shutdown(a);
  aud_player_stop(&a->player);
  aud_preview_stop(&a->preview);
  app_close_engine(a);
  aud_clipboard_clear(&a->clipboard);
  aud_doc_free(&a->doc);
  aud_device_watch_destroy(a->watch);
  CloseWindow();
  app_free(a);
  plug = NULL;
}

/* -- being replaced --------------------------------------------------------- */

/*
 * The library is about to be thrown away, and the state has to be able to
 * survive being handed to a differently built copy of this code.
 *
 * So everything only this build knows the shape of is let go here, while the
 * code that made it is still the code doing the letting go: the analyser and
 * the encoder are both freed by their own version and stood back up by the
 * next one, which is what makes viz.c and render.c as freely editable as the
 * drawing they serve.
 *
 * What is left is the session - the tracks, the selection, the undo stack, the
 * device the engine holds - and that is the point of the exercise.
 */
void *aud_plug_pre_reload(void)
{
  app *a = plug;

  /* the ffmpeg pipe and the render target it draws into, both this build's */
  app_cancel_render(a);

  aud_viz_destroy(a->viz);
  a->viz = NULL;

  return a;
}

bool aud_plug_post_reload(void *state)
{
  app *a = state;

  /*
   * The one thing that cannot be papered over: this build lays `app` out
   * differently from the build that packed the session, so every field in it
   * is somewhere else and there is no honest way to carry on. Reading
   * self_size is safe whatever else moved - it is the first field of the
   * struct, and both builds agree on where the front of an allocation is.
   */
  if (a->self_size != sizeof(app))
  {
    aud_error("the window's state is %zu bytes and this build wants %zu: app.h "
              "changed, and a session cannot be carried across that. Restart it.",
              a->self_size, sizeof(app));
    return false;
  }

  plug = a;
  app_name_styles(a);

  if (a->engine == NULL)
  {
    return true; /* the "no device" screen; there is nothing to analyse yet */
  }

  a->viz = aud_viz_create(aud_engine_rate(a->engine), AUD_VIZ_DEFAULT_BANDS);
  if (a->viz == NULL)
  {
    /*
     * The window draws from the engine and the analyser together and cannot
     * have one without the other, so this drops back to the screen that has
     * neither rather than to a frame that would read through a null.
     */
    snprintf(a->fatal, sizeof(a->fatal), "cannot set up the spectrum display");
    app_close_engine(a);
    return true;
  }

  aud_viz_set_mode(a->viz, (aud_viz_mode)a->style_selected);
  app_set_status(a, "reloaded");
  return true;
}
