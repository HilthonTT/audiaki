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
 * happens every frame, and what happens on the way out - along with the
 * engine's lifecycle and the transport actions the buttons and keys stand for.
 * app.h has the state these files share and says which of them does what.
 *
 * It is also where the app meets the shell that runs it: the four entry points
 * at the bottom are the whole of that boundary, and in a development build
 * everything in this directory except main.c and engine.c can be rebuilt and
 * loaded again behind them without the window closing. See hotreload/plug.h.
 */
#include "gui/app.h"

#include "gui/engine.h"
#include "gui/render.h"
#include "gui/ui.h"
#include "gui/viz.h"

#include "hotreload/plug.h"

#include "audio/format.h"
#include "backend/backend.h"
#include "backend/device.h"
#include "backend/monitor.h"
#include "edit/edit.h"
#include "edit/export.h"
#include "edit/load.h"
#include "edit/project.h"
#include "take/latency.h"
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
  a->project_dirty = 1;

  app_set_status(a, "%.80s: %.1f s on track %d", aud_path_basename(path),
                 (double)aud_track_end(&a->doc.tracks[index]) / a->doc.rate, index + 1);
}

/* Open the device and build the display for whatever it negotiated. */
static int app_open_engine(app *a)
{
  unsigned channels;

  a->fatal[0] = '\0';
  a->take_buf_frames = 0;

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

  /*
   * The drain is a fixed block of samples, so how many frames of it can be
   * asked for is whatever this device's channel count divides into. Asking for
   * more than that would have the engine write the extra channels past the end.
   */
  channels = aud_engine_channels(a->engine);
  a->take_buf_frames = channels > 0 ? APP_TAKE_BUF_SAMPLES / channels : 0;

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
  a->take_buf_frames = 0;
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

/* Defined below, next to the rest of what a take does. */
static void app_resume_take(app *a);

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
    app_resume_take(a);
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
/*
 * Frames to shift a take back by, so what was played along to the project lands
 * where it was heard rather than a round trip after it.
 *
 * Only ever asked once playback has actually started. A correction is a guess
 * about a delay, and applying one when there is nothing being played to be late
 * against would move a take away from the line the user put it on for no reason
 * at all - which is worse than the misalignment it is trying to fix.
 */
static uint64_t app_latency_frames(const app *a)
{
  aud_monitor_config out;

  if (a->engine == NULL)
  {
    return 0;
  }

  /* the output the player opened, so its buffer is known from the same
   * defaults monitor.c uses rather than from a second copy of them */
  aud_monitor_config_defaults(&out, aud_engine_rate(a->engine), 2u);

  return aud_latency_frames(a->latency_ms, aud_engine_rate(a->engine),
                            aud_engine_capture_frames(a->engine),
                            (unsigned long)out.period_frames * out.periods);
}

void app_begin_take(app *a)
{
  char path[AUD_ENGINE_PATH_MAX];
  uint64_t at;
  uint64_t start;
  uint64_t skip;
  uint64_t latency;
  long target;
  int along;

  if (a->engine == NULL || aud_take_next(path, sizeof(path), a->prefix) != 0)
  {
    return;
  }

  aud_player_stop(&a->player);

  at = a->doc.cursor;

  /*
   * Playback first, because whether it started is what decides where the take
   * goes. Playing along to something means hearing it late, whether that
   * something is the project or the metronome counting over an empty
   * timeline - so anything that opens an output is something the take has to
   * be shifted back against. Nothing to play along to, or an output that will
   * not open, and the take belongs exactly on the line where it was asked for.
   */
  latency = 0;
  along = a->overdub && aud_doc_end(&a->doc) > at;

  if (along || a->click_on)
  {
    /* the project only when it was asked for; the click runs either way, and
     * past the end of what is there, so the pass has no end of its own */
    aud_player_set_mix(&a->player, along);
    app_apply_transport(a);
    aud_player_set_loop(&a->player, 0);

    if (aud_player_start(&a->player, &a->doc, at,
                         a->click_on ? AUD_PLAYER_OPEN_ENDED : aud_doc_end(&a->doc),
                         a->cfg.monitor_device) == 0)
    {
      latency = app_latency_frames(a);
    }
  }

  aud_latency_place(at, latency, &start, &skip);

  /* the lane has to be free from where the clip really begins, which with a
   * correction applied is earlier than the cursor */
  target = app_record_target(a, start);
  if (target < 0)
  {
    aud_player_stop(&a->player);
    app_set_status(a, "no room for another track");
    return;
  }

  if (aud_track_record_begin(&a->doc.tracks[target], start,
                             (size_t)aud_engine_rate(a->engine) * 8u) != 0)
  {
    aud_player_stop(&a->player);
    app_set_status(a, "there is already audio there - move the cursor");
    return;
  }

  a->render_note[0] = '\0';
  a->record_track = target;
  a->record_at = start;
  a->record_skip = skip;

  if (aud_engine_start(a->engine, path, 0) != 0)
  {
    aud_player_stop(&a->player);
    aud_track_record_end(&a->doc.tracks[target]);
    a->record_track = -1;
    a->record_skip = 0;
    return;
  }

  if (latency > 0)
  {
    double back = 1000.0 * (double)latency / aud_engine_rate(a->engine);

    if (along)
    {
      app_set_status(a, "overdubbing %.50s (%.0f ms back)", aud_path_basename(path),
                     back);
    }
    else
    {
      app_set_status(a, "recording %.40s to the click (%.0f ms back)",
                     aud_path_basename(path), back);
    }
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
  unsigned channels;
  size_t got;

  if (a->record_track < 0 || (size_t)a->record_track >= a->doc.count)
  {
    return;
  }

  channels = aud_engine_channels(a->engine);

  while ((got = aud_engine_read_take(a->engine, a->take_buf, a->take_buf_frames)) > 0)
  {
    const float *frames = a->take_buf;
    size_t take = got;

    /*
     * The head of a take that the latency correction could not shift into,
     * because there was no timeline before frame zero to shift it into. Those
     * frames describe a moment the project does not have.
     */
    if (a->record_skip > 0)
    {
      size_t drop = a->record_skip < take ? (size_t)a->record_skip : take;

      frames += drop * channels;
      take -= drop;
      a->record_skip -= drop;
    }

    if (take > 0)
    {
      aud_track_record_push(&a->doc.tracks[a->record_track], frames, take);
      a->doc.dirty = 1;
    }

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

  /* stopping stops the transport, overdub and all */
  aud_player_stop(&a->player);

  /* whatever was still in flight when the take closed belongs on the track */
  app_pump_take(a);

  a->project_dirty = 1;

  if (a->record_track >= 0 && (size_t)a->record_track < a->doc.count)
  {
    aud_track *t = &a->doc.tracks[a->record_track];

    /*
     * Tell the block which file it is, while the clip that holds it is still
     * open. The timeline's copy of a take and the WAV beside it are the same
     * audio, and a project saved later refers to the file rather than carrying
     * the samples - see edit/project.h.
     */
    if (aud_track_recording(t))
    {
      aud_samples_set_source(t->clips[t->recording].audio, take);
    }
    a->last_take_track = a->record_track;

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
      /* the reload appended it, and it brought its own source with it */
      a->last_take_track = (long)a->doc.count - 1;
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
  a->record_skip = 0;
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
 * How long a device has to come back within for the take to be carried on.
 *
 * A cable knocked out and pushed back in is somebody still standing there with
 * an instrument, and picking the take up where it stopped is what they want. An
 * interface plugged in again after lunch is not, and a window that started
 * recording to disk on its own because of something that happened half an hour
 * ago would be a window nobody could leave running.
 */
#define APP_RESUME_SECONDS 30.0

/*
 * The capture stream died while a take was being written.
 *
 * The file is already safe - the engine closes it the moment the stream goes,
 * which is the whole of what salvaging it means - but the timeline still has a
 * clip open on it, growing from a source that has stopped. Close that the way
 * stopping normally would, and note where it got to, so a device that comes
 * back can be carried on from rather than started over.
 *
 * No dialog. The question of where a take should be kept is asked about a take
 * that is over, and this one may yet have a second half; it stays where it was
 * recorded, and the status line says so.
 */
static void app_take_interrupted(app *a, const aud_engine_status *st)
{
  char take[AUD_ENGINE_PATH_MAX];
  double seconds = st->elapsed;
  unsigned long dropped = aud_engine_take_dropped(a->engine);

  snprintf(take, sizeof(take), "%s", st->path);

  /* whatever it was being played along to has nothing left to accompany */
  aud_player_stop(&a->player);

  /* whatever reached the ring before the stream went belongs on the track */
  app_pump_take(a);

  a->project_dirty = 1;
  memset(&a->interrupted, 0, sizeof(a->interrupted));
  a->interrupted.track = -1;

  if (a->record_track >= 0 && (size_t)a->record_track < a->doc.count)
  {
    aud_track *t = &a->doc.tracks[a->record_track];

    if (aud_track_recording(t))
    {
      aud_samples_set_source(t->clips[t->recording].audio, take);
    }
    a->last_take_track = a->record_track;
    aud_track_record_end(t);

    if (dropped > 0)
    {
      /*
       * The display fell behind as well as the device going, so the track is
       * not what the file is. Rebuilt from the file, like a take that stopped
       * normally - and not offered to be carried on, because the frame the
       * reload lands on is not the frame the take stopped at.
       */
      aud_doc_remove_track(&a->doc, (size_t)a->record_track);
      a->record_track = -1;
      app_load_track(a, take);
      a->last_take_track = (long)a->doc.count - 1;
    }
    else
    {
      snprintf(t->name, sizeof(t->name), "%s", aud_path_basename(take));

      a->interrupted.waiting = 1;
      a->interrupted.track = a->record_track;
      /* one past the last frame that arrived, so a second half butts up
       * against the first rather than being refused for overlapping it */
      a->interrupted.at = aud_track_end(t);
      a->interrupted.lost_at = GetTime();
      a->interrupted.rate = aud_engine_rate(a->engine);
      a->interrupted.channels = aud_engine_channels(a->engine);
    }
  }

  a->record_track = -1;
  a->record_skip = 0;
  a->render_note[0] = '\0';

  app_set_status(a, "the device went during %.40s - %.1f s kept%s",
                 aud_path_basename(take), seconds,
                 a->interrupted.waiting ? "; plug it back in to carry on" : "");
}

/*
 * Notice that the stream died under a take, once per frame, before anything
 * else looks at the timeline.
 *
 * Ahead of the device watch in particular: the engine that was writing the
 * take is the only thing that can still be asked what it wrote, and the watch
 * is what tears it down and stands a new one up in its place.
 */
static void app_check_capture_loss(app *a)
{
  aud_engine_status st;

  if (a->engine == NULL || a->record_track < 0)
  {
    return;
  }

  aud_engine_status_get(a->engine, &st);
  if (st.state == AUD_ENGINE_FAILED)
  {
    app_take_interrupted(a, &st);
  }
}

/*
 * Carry the interrupted take on, now that there is a device again.
 *
 * Everything about it has to still be true: the lane is still there, the new
 * device delivers what the old one did, and it has not been so long that
 * nobody is waiting. Declining costs nothing - what was captured is a clip and
 * a WAV either way, and Record starts the next take wherever the cursor is.
 *
 * The second half is a take of its own, in a file of its own, laid on the same
 * lane where the first one stopped. Splicing them into one file would mean
 * rewriting a header on a recording that is already safe on disk, to make a
 * file whose middle is a moment the interface was not running.
 */
static void app_resume_take(app *a)
{
  char path[AUD_ENGINE_PATH_MAX];
  aud_track *t;

  if (!a->interrupted.waiting || a->engine == NULL)
  {
    return;
  }

  a->interrupted.waiting = 0;

  if (GetTime() - a->interrupted.lost_at > APP_RESUME_SECONDS)
  {
    app_set_status(a, "'%.40s' is back - press Record for the next take",
                   a->active_device);
    return;
  }

  if (a->interrupted.track < 0 || (size_t)a->interrupted.track >= a->doc.count)
  {
    return; /* the lane was edited away while the device was gone */
  }

  /*
   * A device that comes back is not necessarily the device that went, and one
   * running at another rate or another width cannot be laid onto the end of a
   * lane recorded at the old one.
   */
  if (aud_engine_rate(a->engine) != a->interrupted.rate ||
      aud_engine_channels(a->engine) != a->interrupted.channels)
  {
    app_set_status(a,
                   "'%.30s' is back, but at %u Hz / %u ch - press Record to "
                   "start a take on it",
                   a->active_device, aud_engine_rate(a->engine),
                   aud_engine_channels(a->engine));
    return;
  }

  if (aud_take_next(path, sizeof(path), a->prefix) != 0)
  {
    return;
  }

  t = &a->doc.tracks[a->interrupted.track];
  if (aud_track_record_begin(t, a->interrupted.at,
                             (size_t)aud_engine_rate(a->engine) * 8u) != 0)
  {
    return;
  }

  a->record_track = a->interrupted.track;
  a->record_at = a->interrupted.at;
  a->record_skip = 0;
  a->render_note[0] = '\0';

  if (aud_engine_start(a->engine, path, 0) != 0)
  {
    aud_track_record_end(t);
    a->record_track = -1;
    return;
  }

  app_set_status(a, "'%.30s' is back - carrying on into %.40s", a->active_device,
                 aud_path_basename(path));
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

  /*
   * Wherever the take ended up is where its block now says it lives. The
   * dialog may have moved the file since it was stamped, and a project saved
   * afterwards has to point at the take rather than at the name it was
   * recorded under.
   */
  if (a->last_take_track >= 0 && (size_t)a->last_take_track < a->doc.count &&
      take[0] != '\0')
  {
    const aud_track *t = &a->doc.tracks[a->last_take_track];

    for (size_t c = 0; c < t->count; c++)
    {
      aud_samples_set_source(t->clips[c].audio, take);
    }
  }

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
    a->project_dirty = a->project_dirty || ok == 0;
    app_set_status(a, ok == 0 ? "undone" : "nothing to undo");
    return;
  case APP_EDIT_REDO:
    ok = aud_doc_redo(d);
    a->project_dirty = a->project_dirty || ok == 0;
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
  case APP_EDIT_FADE_IN:
    ok = aud_edit_fade_in(d);
    break;
  case APP_EDIT_FADE_OUT:
    ok = aud_edit_fade_out(d);
    break;
  default:
    return;
  }

  if (ok == 0)
  {
    static const char *const done[] = {
        "",         "",        "cut",   "copied",     "pasted",        "deleted",
        "silenced", "trimmed", "split", "duplicated", "faded in over", "faded out over"};

    /* the session has moved away from whatever is on disk, if anything is */
    a->project_dirty = 1;
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
void app_apply_transport(app *a)
{
  aud_player_set_click(&a->player, a->click_on ? a->doc.tempo : 0.0, a->doc.beats_per_bar,
                       a->click_gain);
  /*
   * Never while a take is open. A loop means playing the same seconds over
   * and over, and a recording made against one would be a single straight
   * take laid over music that repeated underneath it.
   */
  aud_player_set_loop(&a->player, a->loop && a->record_track < 0);
}

void app_nudge_tempo(app *a, double beats)
{
  aud_doc_set_tempo(&a->doc, a->doc.tempo + beats, a->doc.beats_per_bar);
  a->project_dirty = 1;
  app_apply_transport(a);
  app_set_status(a, "%.0f BPM, %u to the bar", a->doc.tempo, a->doc.beats_per_bar);
}

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

  /*
   * With the metronome on there is always something to hear, even with an
   * empty timeline: playing a bar in before the first take is exactly what a
   * count-in is, and refusing it because no audio exists yet would be
   * refusing the one thing it is for.
   */
  if (a->doc.count == 0 && !a->click_on)
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

  /*
   * A click over an empty stretch has nothing to bound it, so it runs until
   * it is stopped. Anything with audio in it plays that audio, looped or not.
   */
  if (to <= from)
  {
    to = AUD_PLAYER_OPEN_ENDED;
  }

  aud_player_set_mix(&a->player, 1);
  app_apply_transport(a);

  if (aud_player_start(&a->player, &a->doc, from, to, a->cfg.monitor_device) != 0)
  {
    app_set_status(a, "cannot open an output to play through");
    return;
  }

  if (to == AUD_PLAYER_OPEN_ENDED)
  {
    app_set_status(a, "counting at %.0f BPM", a->doc.tempo);
    return;
  }

  if (a->loop)
  {
    app_set_status(a, "looping %.2f s", (double)(to - from) / a->doc.rate);
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
 * How far one press of an arrow moves the cursor: the time a few pixels covers
 * at the current zoom.
 *
 * Not a fixed number of seconds, because there is no one right answer to that.
 * Zoomed out to a whole session an arrow should cover ground; zoomed in on a
 * transient it should land on a sample. A distance in pixels is the same
 * gesture at both, and is the one the eye is judging anyway.
 */
#define APP_NUDGE_PIXELS 8.0

static uint64_t app_nudge(const app *a)
{
  double frames;

  if (a->doc.rate == 0 || !(a->timeline.zoom > 0.0))
  {
    return 1;
  }

  frames = APP_NUDGE_PIXELS / a->timeline.zoom * (double)a->doc.rate;
  return frames >= 1.0 ? (uint64_t)(frames + 0.5) : 1u;
}

/* Frames from `at`, without running off the front of the timeline. */
static uint64_t app_step(uint64_t at, uint64_t by, int back)
{
  if (!back)
  {
    return at + by;
  }
  return at > by ? at - by : 0;
}

/*
 * The end of the selection an arrow is moving. The cursor sits on the anchor -
 * see aud_doc_select_from() - so the other end is whichever one it is not, and
 * with no range the two are the same place.
 */
static uint64_t app_moving_edge(const aud_doc *d)
{
  if (d->sel_end <= d->sel_start)
  {
    return d->cursor;
  }
  return d->cursor == d->sel_start ? d->sel_end : d->sel_start;
}

/* The nearest clip edge either way, across the selected tracks - or all of
 * them when none is selected, so the keys work before anything is picked. */
static uint64_t app_clip_edge(const app *a, uint64_t from, int back)
{
  int only_selected = aud_doc_any_track_selected(&a->doc);
  uint64_t best = from;

  for (size_t i = 0; i < a->doc.count; i++)
  {
    const aud_track *t = &a->doc.tracks[i];
    uint64_t edge;

    if (only_selected && !t->selected)
    {
      continue;
    }

    edge = back ? aud_track_edge_before(t, from) : aud_track_edge_after(t, from);
    if (edge == from)
    {
      continue; /* nothing that way on this lane */
    }
    if (best == from || (back ? edge > best : edge < best))
    {
      best = edge;
    }
  }
  return best;
}

/* Move the cursor, or the end of the selection when `extend` is set. */
static void app_move_cursor(app *a, uint64_t to, int extend)
{
  if (extend)
  {
    aud_doc_select_from(&a->doc, a->doc.cursor, to);
  }
  else
  {
    aud_doc_set_cursor(&a->doc, to);
  }

  aud_timeline_reveal(&a->timeline, &a->doc, to,
                      (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W -
                          AUD_TIMELINE_SCALE_W);
}

/*
 * Walk the track selection up or down the stack, so the lanes an edit reaches
 * can be chosen without the pointer. Shift adds rather than replaces, which is
 * what ctrl+click does with the mouse.
 */
static void app_step_track(app *a, int down, int add)
{
  size_t first = a->doc.count;
  size_t last = a->doc.count;
  size_t to;

  if (a->doc.count == 0)
  {
    return;
  }

  for (size_t i = 0; i < a->doc.count; i++)
  {
    if (a->doc.tracks[i].selected)
    {
      first = first == a->doc.count ? i : first;
      last = i;
    }
  }

  if (first == a->doc.count)
  {
    to = down ? 0 : a->doc.count - 1u; /* nothing selected: start at the near end */
  }
  else if (down)
  {
    to = last + 1u < a->doc.count ? last + 1u : last;
  }
  else
  {
    to = first > 0 ? first - 1u : first;
  }

  if (!add)
  {
    aud_doc_select_tracks(&a->doc, 0);
  }
  a->doc.tracks[to].selected = 1;
  a->doc.dirty = 1;

  /* the room the lanes actually got at the last draw, which only it knows */
  aud_timeline_reveal_track(&a->timeline, &a->doc, to, a->timeline.rows_h);
}

/*
 * Everything the arrow keys do. Left and right are time - a nudge, a clip edge
 * with ctrl, the whole project with home and end - and up and down are which
 * lanes are selected. Shift extends the selection instead of moving the cursor,
 * as it does in every editor with a keyboard.
 */
static void handle_arrows(app *a, int ctrl, int shift)
{
  int left = IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT);
  int right = IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT);

  if (left || right)
  {
    uint64_t from;
    uint64_t to;

    /*
     * A bare arrow over a selection puts it down at the end it is heading for,
     * as it does over selected text, rather than setting off from the far side
     * of a range the eye is looking at the near side of.
     */
    if (!shift && aud_doc_has_range(&a->doc))
    {
      app_move_cursor(a, left ? a->doc.sel_start : a->doc.sel_end, 0);
    }
    else
    {
      from = shift ? app_moving_edge(&a->doc) : a->doc.cursor;
      to = ctrl ? app_clip_edge(a, from, left) : app_step(from, app_nudge(a), left);
      app_move_cursor(a, to, shift);
    }
  }

  if (IsKeyPressed(KEY_HOME))
  {
    app_move_cursor(a, 0, shift);
  }
  if (IsKeyPressed(KEY_END))
  {
    app_move_cursor(a, aud_doc_end(&a->doc), shift);
  }

  if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
  {
    app_step_track(a, 1, shift);
  }
  if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
  {
    app_step_track(a, 0, shift);
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
    /* the session itself: S writes it, shift+S asks where, O opens one */
    if (IsKeyPressed(KEY_S))
    {
      if (shift)
      {
        app_save_project_as(a);
      }
      else
      {
        app_save_project(a);
      }
    }
    if (IsKeyPressed(KEY_O))
    {
      app_open_project_dialog(a);
    }
    /* ctrl+arrow steps between clip edges rather than by a nudge */
    handle_arrows(a, 1, shift);
    /* the transport, where the editor's own space bar has displaced it */
    if (IsKeyPressed(KEY_SPACE))
    {
      app_toggle_record(a, st);
    }
    return;
  }

  handle_arrows(a, 0, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));

  /* the fades, on the bracket keys the selection edges look like */
  if (IsKeyPressed(KEY_LEFT_BRACKET))
  {
    app_edit(a, APP_EDIT_FADE_IN);
  }
  if (IsKeyPressed(KEY_RIGHT_BRACKET))
  {
    app_edit(a, APP_EDIT_FADE_OUT);
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

  if (IsKeyPressed(KEY_I))
  {
    app_open_dialog(a);
  }

  if (IsKeyPressed(KEY_B))
  {
    a->viz_open = !a->viz_open;
  }

  /* the three that count time: the loop, the metronome and the grid it beats on */
  if (IsKeyPressed(KEY_L))
  {
    a->loop = !a->loop;
    app_apply_transport(a);
    app_set_status(a, "%s", a->loop ? "looping" : "playing straight through");
  }

  if (IsKeyPressed(KEY_C))
  {
    a->click_on = !a->click_on;
    app_apply_transport(a);
    if (a->click_on)
    {
      app_set_status(a, "metronome on at %.0f BPM", a->doc.tempo);
    }
    else
    {
      app_set_status(a, "metronome off");
    }
  }

  if (IsKeyPressed(KEY_G))
  {
    a->timeline.grid = !a->timeline.grid;
    app_set_status(a, "%s", a->timeline.grid ? "grid on - alt steps off it" : "grid off");
  }

  /* the tempo, without having to reach for the spinner */
  if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
  {
    app_nudge_tempo(a,
                    IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 10.0 : 1.0);
  }
  if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
  {
    app_nudge_tempo(a, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? -10.0
                                                                               : -1.0);
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
  a->viz_open = 1; /* what the window has always come up showing */
  a->viz_height = APP_VIZ_OPEN_H;
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

void aud_plug_frame(void)
{
  app *a = plug;
  aud_engine_status st;

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

    BeginDrawing();
    app_draw_fatal(a);
    EndDrawing();
    return;
  }

  aud_engine_status_get(a->engine, &st);

  SetMouseCursor(MOUSE_CURSOR_DEFAULT);
  handle_keys(a, &st);
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
                        (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W -
                            AUD_TIMELINE_SCALE_W);
  }
  /* a take being recorded scrolls into view the same way */
  else if (a->record_track >= 0)
  {
    aud_timeline_reveal(
        &a->timeline, &a->doc, a->record_at + (uint64_t)(st.elapsed * a->doc.rate),
        (float)GetScreenWidth() - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W);
  }
  app_track_peak(a, (float)st.peak, GetFrameTime());

  /*
   * Before BeginDrawing: the renderer has its own render target to bind, and
   * doing that inside the window's pass would leave the wrong one current.
   */
  app_pump_render(a);
  app_update_title(a, &st);

  BeginDrawing();
  app_draw_frame(a, &st);
  EndDrawing();
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
