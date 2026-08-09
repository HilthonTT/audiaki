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
#include "edit/edit.h"
#include "edit/export.h"
#include "edit/load.h"
#include "take/take.h"
#include "util/config.h"
#include "util/log.h"
#include "util/path.h"
#include "version.h"

#include <math.h>
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

/*
 * Bring a finished WAV in as a track.
 *
 * Read on the drawing thread, which for a take of the length anyone plays in
 * one go is a hitch rather than a freeze - a minute of stereo is about ten
 * megabytes and arrives in well under a frame's worth of patience. A session
 * long enough for that to matter is one this editor is already the wrong tool
 * for, and the ceiling in edit/load.c is where that is said.
 */
void app_load_track(app *a, const char *path)
{
  const char *why = NULL;
  int index;

  index = aud_edit_load_wav(&a->doc, path, &why);
  if (index < 0)
  {
    app_set_status(a, "cannot open %.80s: %s", aud_path_basename(path),
                   why != NULL ? why : "unknown");
    return;
  }

  /* the new lane is the selected one, so an edit typed straight after landing
   * a take applies to the take rather than to nothing */
  aud_doc_select_tracks(&a->doc, 0);
  a->doc.tracks[index].selected = 1;

  app_set_status(a, "%.80s: %.1f s on track %d", aud_path_basename(path),
                 (double)aud_track_end(&a->doc.tracks[index]) / a->doc.rate, index + 1);
}

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
 * Where the take about to be recorded should land.
 *
 * At the cursor, on the selected track if there is room for it there, and on a
 * new one otherwise. That is what "click somewhere and press record again"
 * means: the second take goes where you pointed, next to the first rather than
 * over it, and a lane that is already busy at that moment gets a lane of its
 * own rather than a refusal.
 */
static long app_record_target(app *a, uint64_t at)
{
  aud_track *fresh;
  char name[AUD_TRACK_NAME_MAX];
  unsigned channels = aud_engine_channels(a->engine);

  for (size_t i = 0; i < a->doc.count; i++)
  {
    aud_track *t = &a->doc.tracks[i];

    if (t->selected && t->channels == channels && aud_track_end(t) <= at)
    {
      return (long)i;
    }
  }

  snprintf(name, sizeof(name), "Take %zu", a->doc.count + 1u);
  fresh = aud_doc_add_track(&a->doc, name, channels);
  if (fresh == NULL)
  {
    return -1;
  }

  aud_doc_select_tracks(&a->doc, 0);
  fresh->selected = 1;
  return (long)(a->doc.count - 1u);
}

/*
 * Pick the next free take name and start writing to it. Numbering rather than
 * prompting: pressing record should never be the moment you find out you are
 * about to overwrite yesterday's take.
 *
 * The file and the timeline start together. What goes into one goes into the
 * other, frame for frame, so what is on screen while you play is what is on
 * disk when you stop.
 */
void app_begin_take(app *a)
{
  char path[AUD_ENGINE_PATH_MAX];
  uint64_t at;
  long target;

  if (a->engine == NULL || aud_take_next(path, sizeof(path), a->prefix) != 0)
  {
    return;
  }

  /* playback and recording at once is overdubbing, which needs the two clocks
   * lined up; until then, one at a time */
  aud_player_stop(&a->player);

  at = a->doc.cursor;
  target = app_record_target(a, at);
  if (target < 0)
  {
    app_set_status(a, "no room for another track");
    return;
  }

  if (aud_track_record_begin(&a->doc.tracks[target], at,
                             (size_t)aud_engine_rate(a->engine) * 8u) != 0)
  {
    app_set_status(a, "there is already audio there - move the cursor");
    return;
  }

  a->render_note[0] = '\0';
  a->record_track = target;
  a->record_at = at;

  if (aud_engine_start(a->engine, path, 0) != 0)
  {
    aud_track_record_end(&a->doc.tracks[target]);
    a->record_track = -1;
    return;
  }

  app_set_status(a, "recording %.60s", aud_path_basename(path));
}

/*
 * Move whatever the engine has captured onto the track being recorded into.
 * Called every drawn frame, which is what makes the waveform grow as it is
 * played rather than appear when it stops.
 */
static void app_pump_take(app *a)
{
  size_t got;

  if (a->record_track < 0 || (size_t)a->record_track >= a->doc.count)
  {
    return;
  }

  while ((got = aud_engine_read_take(a->engine, a->take_buf, a->take_buf_frames)) > 0)
  {
    aud_track_record_push(&a->doc.tracks[a->record_track], a->take_buf, got);
    a->doc.dirty = 1;
    if (got < a->take_buf_frames)
    {
      break;
    }
  }
}

/*
 * Stop the take, and then deal with what it left behind. The WAV has to be
 * closed first, before either of those: ffmpeg opens it to read the audio, and
 * a header that has not been patched yet describes a file of zero length.
 */
void app_stop_take(app *a, const aud_engine_status *st)
{
  char take[AUD_ENGINE_PATH_MAX];
  double seconds = st->elapsed;
  unsigned long dropped;

  snprintf(take, sizeof(take), "%s", st->path);
  dropped = aud_engine_take_dropped(a->engine);

  if (aud_engine_stop(a->engine) != 0)
  {
    return;
  } /* the failure is already in the status line */

  /* whatever was still in flight when the take closed belongs on the track */
  app_pump_take(a);

  if (a->record_track >= 0 && (size_t)a->record_track < a->doc.count)
  {
    aud_track *t = &a->doc.tracks[a->record_track];

    aud_track_record_end(t);

    /*
     * The file is what the take is; the track was a view of it arriving. They
     * agree unless the drawing thread fell so far behind that the ring
     * overflowed, and in that one case the track is rebuilt from the file
     * rather than left quietly wrong.
     */
    if (dropped > 0)
    {
      aud_doc_remove_track(&a->doc, (size_t)a->record_track);
      a->record_track = -1;
      app_load_track(a, take);
      app_set_status(a, "the display fell behind; the take was reloaded from disk");
      a->record_track = -1;
    }
    else
    {
      snprintf(t->name, sizeof(t->name), "%s", aud_path_basename(take));
      app_set_status(a, "%.60s: %.1f s", aud_path_basename(take), seconds);
    }
  }
  a->record_track = -1;
  a->render_note[0] = '\0';

  /*
   * The video waits for the dialog rather than starting beside it: it is
   * rendered from the take, and it should be rendered from wherever the take
   * ends up rather than from where it happened to be written.
   */
  if (a->want_dialog && take[0] != '\0')
  {
    app_save_open(a, take, seconds);
    return;
  }

  app_finish_take(a, take);
}

/*
 * The take is where it is going to stay. Render its video, if one was asked
 * for; it is already on the timeline, having grown there while it was played.
 */
void app_finish_take(app *a, const char *path)
{
  aud_render_options opts;
  char video[AUD_RENDER_PATH_MAX];
  char take[AUD_ENGINE_PATH_MAX];

  snprintf(take, sizeof(take), "%s", path);

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

/*
 * Every edit, in one place, so the toolbar and the keyboard cannot drift apart
 * about what any of them means or what it says afterwards.
 *
 * The operations themselves refuse when there is nothing selected, and saying
 * so is most of what this adds: a button that appears to do nothing is a bug
 * report, and "select some audio first" is the answer to it.
 */
void app_edit(app *a, app_edit_action action)
{
  aud_doc *d = &a->doc;
  int ok = -1;

  switch (action)
  {
  case APP_EDIT_UNDO:
    ok = aud_doc_undo(d);
    app_set_status(a, ok == 0 ? "undone" : "nothing to undo");
    return;
  case APP_EDIT_REDO:
    ok = aud_doc_redo(d);
    app_set_status(a, ok == 0 ? "redone" : "nothing to redo");
    return;
  case APP_EDIT_SELECT_ALL:
    aud_doc_select_all(d);
    app_set_status(a, "everything selected");
    return;
  case APP_EDIT_CUT:
    ok = aud_edit_cut(d, &a->clipboard);
    break;
  case APP_EDIT_COPY:
    ok = aud_edit_copy(d, &a->clipboard);
    break;
  case APP_EDIT_PASTE:
    ok = aud_edit_paste(d, &a->clipboard);
    break;
  case APP_EDIT_DELETE:
    ok = aud_edit_delete(d);
    break;
  case APP_EDIT_SILENCE:
    ok = aud_edit_silence(d);
    break;
  case APP_EDIT_TRIM:
    ok = aud_edit_trim(d);
    break;
  case APP_EDIT_SPLIT:
    ok = aud_edit_split(d);
    break;
  case APP_EDIT_DUPLICATE:
    ok = aud_edit_duplicate(d);
    break;
  default:
    return;
  }

  if (ok == 0)
  {
    static const char *const done[] = {"",       "",          "cut",      "copied",
                                       "pasted", "deleted",   "silenced", "trimmed",
                                       "split",  "duplicated"};

    app_set_status(a, "%s %.2f s", done[action],
                   d->rate > 0 ? (double)(d->sel_end - d->sel_start) / d->rate : 0.0);
    return;
  }

  /* why it refused, which is nearly always one of these two */
  if (action == APP_EDIT_PASTE && aud_clipboard_empty(&a->clipboard))
  {
    app_set_status(a, "nothing on the clipboard");
  }
  else if (!aud_doc_any_track_selected(d))
  {
    app_set_status(a, "click a track first");
  }
  else
  {
    app_set_status(a, "select some audio first - click and drag across a track");
  }
}

/*
 * Play, or stop playing.
 *
 * From the start of the selection to its end when there is one, and from the
 * cursor to the end of the project when there is not - which between them are
 * the two things anyone means by pressing play in an editor: "let me hear that
 * bit" and "let me hear the rest".
 */
void app_toggle_play(app *a)
{
  uint64_t from;
  uint64_t to;

  if (aud_player_playing(&a->player))
  {
    aud_player_stop(&a->player);
    app_set_status(a, "stopped");
    return;
  }

  if (a->doc.count == 0)
  {
    app_set_status(a, "nothing to play yet");
    return;
  }

  from = aud_doc_has_range(&a->doc) ? a->doc.sel_start : a->doc.cursor;
  to = aud_doc_has_range(&a->doc) ? a->doc.sel_end : aud_doc_end(&a->doc);

  if (to <= from)
  {
    /* past the end: start again rather than do nothing and look broken */
    from = 0;
    to = aud_doc_end(&a->doc);
  }

  if (aud_player_start(&a->player, &a->doc, from, to, a->cfg.monitor_device) != 0)
  {
    app_set_status(a, "cannot open an output to play through");
    return;
  }

  app_set_status(a, "playing %.2f s", (double)(to - from) / a->doc.rate);
}

void app_export(app *a, const char *path)
{
  aud_export_options opts;
  const char *why = NULL;

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1; /* the browser already asked, and said so if it existed */

  /*
   * The selection when there is one. Exporting a chorus you have just cut down
   * to should not mean exporting the whole session and cutting it again in
   * something else.
   */
  if (aud_doc_has_range(&a->doc))
  {
    opts.from = a->doc.sel_start;
    opts.to = a->doc.sel_end;
  }

  if (aud_export_wav(&a->doc, &opts, &why) != 0)
  {
    app_set_status(a, "cannot export: %s", why != NULL ? why : "unknown");
    return;
  }

  app_set_status(a, "wrote %.80s", aud_path_basename(path));
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
  /*
   * The dialog is asking where the last take goes and has fields to type into,
   * so it takes the keyboard outright - and takes it first, because a window
   * where R starts a new take while the last one is being named would lose the
   * one being named.
   */
  if (a->save.open)
  {
    return;
  }

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

  /*
   * The edits first, and behind Ctrl, so the single letters the window has
   * always answered to keep meaning what they meant. Ctrl+V is paste and V is
   * the next visualiser style, which is only a collision if the modifier is
   * not looked at.
   */
  if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
  {
    int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (IsKeyPressed(KEY_Z))
    {
      app_edit(a, shift ? APP_EDIT_REDO : APP_EDIT_UNDO);
    }
    if (IsKeyPressed(KEY_Y))
    {
      app_edit(a, APP_EDIT_REDO);
    }
    if (IsKeyPressed(KEY_X))
    {
      app_edit(a, APP_EDIT_CUT);
    }
    if (IsKeyPressed(KEY_C))
    {
      app_edit(a, APP_EDIT_COPY);
    }
    if (IsKeyPressed(KEY_V))
    {
      app_edit(a, APP_EDIT_PASTE);
    }
    if (IsKeyPressed(KEY_A))
    {
      app_edit(a, APP_EDIT_SELECT_ALL);
    }
    if (IsKeyPressed(KEY_D))
    {
      app_edit(a, APP_EDIT_DUPLICATE);
    }
    if (IsKeyPressed(KEY_T))
    {
      app_edit(a, APP_EDIT_TRIM);
    }
    if (IsKeyPressed(KEY_K))
    {
      app_edit(a, APP_EDIT_SPLIT);
    }
    if (IsKeyPressed(KEY_E))
    {
      app_export_dialog(a);
    }
    /* the transport, where the editor's own space bar has displaced it */
    if (IsKeyPressed(KEY_SPACE))
    {
      app_toggle_record(a, st);
    }
    return;
  }

  if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))
  {
    app_edit(a, APP_EDIT_DELETE);
  }

  /*
   * Space plays, the way it does in every editor, and ctrl+space records. The
   * window used to record on space, when there was nothing to play; now that
   * there is, the commoner of the two gets the bare key.
   */
  if (IsKeyPressed(KEY_SPACE))
  {
    if (st->state == AUD_ENGINE_RECORDING || st->state == AUD_ENGINE_PAUSED)
    {
      app_toggle_record(a, st);
    }
    else
    {
      app_toggle_play(a);
    }
  }

  if (IsKeyPressed(KEY_R))
  {
    app_toggle_record(a, st);
  }

  if (IsKeyPressed(KEY_HOME))
  {
    aud_player_stop(&a->player);
    aud_doc_set_cursor(&a->doc, 0);
  }

  if (IsKeyPressed(KEY_I))
  {
    app_open_dialog(a);
  }

  if (IsKeyPressed(KEY_B))
  {
    a->viz_open = !a->viz_open;
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
    else if (aud_player_playing(&a->player))
    {
      aud_player_stop(&a->player);
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

  /*
   * F fits the project to the window, which is what it does in every editor of
   * this kind and what anyone looking at a waveform reaches for. Fullscreen has
   * moved to F11, where a window manager would have put it anyway.
   */
  if (IsKeyPressed(KEY_F))
  {
    float wave = (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W;

    if (aud_doc_has_range(&a->doc))
    {
      aud_timeline_fit_selection(&a->timeline, &a->doc, wave);
    }
    else
    {
      aud_timeline_fit(&a->timeline, &a->doc, wave);
    }
  }

  if (IsKeyPressed(KEY_F11))
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

int main(int argc, char *argv[])
{
  static app a;
  aud_config cfg;
  int rc;

  aud_engine_config_defaults(&a.cfg);
  snprintf(a.prefix, sizeof(a.prefix), "%s", APP_DEFAULT_PREFIX);
  a.monitor_gain = 1.0f;
  a.viz_open = 1; /* what the window has always come up showing */
  a.viz_height = APP_VIZ_OPEN_H;
  aud_timeline_init(&a.timeline);
  aud_clipboard_init(&a.clipboard);
  aud_player_init(&a.player);
  a.record_track = -1;

  /*
   * A quarter of a second of drain per pass. The ring holds four seconds, so
   * this empties it comfortably faster than it fills even on a frame that took
   * far longer than a frame should.
   */
  a.take_buf_frames = 16384u;
  a.take_buf = malloc(a.take_buf_frames * AUD_TAKE_BUF_CHANNELS * sizeof(float));
  if (a.take_buf == NULL)
  {
    aud_error("cannot allocate the take buffer");
    return EXIT_FAILURE;
  }

  /*
   * The same file the CLI reads, and for the same reason: where takes are kept
   * is answered once and then meant every session. Before parse_args, which is
   * what lets --dir and --no-dialog say otherwise.
   */
  aud_config_load(&cfg);
  snprintf(a.take_dir, sizeof(a.take_dir), "%s", cfg.take_dir);
  /*
   * Off unless the config says otherwise, and deliberately the other way round
   * from the terminal recorder. There, a take is a file and the question is
   * where to keep it. Here it lands on the timeline the moment it stops and is
   * ready to be cut about; a dialog between playing something and editing it
   * would be a dialog in the way. Where the WAV itself goes is take_dir's job,
   * and Export is how a finished mix leaves.
   */
  a.want_dialog = cfg.prompt == AUD_PROMPT_ALWAYS;
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

  app_place_prefix(&a);

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

  /*
   * The project takes its rate from whatever the device negotiated, so a take
   * lands on the timeline at the rate it was recorded at rather than being
   * declared to be at some other one.
   */
  aud_doc_init(&a.doc, a.cfg.rate);

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
    aud_doc_init(&a.doc, aud_engine_rate(a.engine));
  }

  /* whatever was named on the command line, now that there is a rate to put
   * it at and a window to say so in if one of them will not open */
  for (int i = 0; i < a.open_count; i++)
  {
    app_load_track(&a, a.open_paths[i]);
  }
  if (a.doc.count > 0)
  {
    aud_timeline_fit(&a.timeline, &a.doc,
                     (float)APP_WIDTH - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W);
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
    app_pump_take(&a);

    /*
     * Before the drawing, so the playhead the frame shows is where playback
     * actually is rather than where it was when the last frame started.
     */
    if (aud_player_pump(&a.player, &a.doc))
    {
      /* it reached the end by itself; leave the cursor where it started, the
       * way a transport does, so pressing play again repeats the same passage */
      app_set_status(&a, "played to the end");
    }
    if (aud_player_playing(&a.player))
    {
      aud_timeline_reveal(&a.timeline, &a.doc, aud_player_head(&a.player),
                          (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W -
                              AUD_TIMELINE_SCALE_W);
    }
    /* a take being recorded scrolls into view the same way */
    else if (a.record_track >= 0)
    {
      aud_timeline_reveal(
          &a.timeline, &a.doc, a.record_at + (uint64_t)(st.elapsed * a.doc.rate),
          (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W);
    }
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
  aud_player_stop(&a.player);
  app_close_engine(&a);
  free(a.take_buf);
  aud_clipboard_clear(&a.clipboard);
  aud_doc_free(&a.doc);
  aud_device_watch_destroy(a.watch);
  CloseWindow();
  return EXIT_SUCCESS;
}
