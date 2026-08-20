/* SPDX-License-Identifier: MIT */
/*
 * take.c - the capture device, and the take being written to it.
 *
 * One file rather than two, because the two are the same story: a take is what
 * the device is for, and the cases worth writing down are all the ones where
 * something happens to one of them in the middle of the other. The cable comes
 * out mid-take, the engine dies, the WAV is salvaged and the clip growing on
 * the timeline has to be closed out against it; the cable goes back in, a new
 * engine stands up, and the take carries on into the file it was already half
 * way through. Separating "the device" from "the take" would put the two halves
 * of that in different files and the reason for either in neither.
 *
 * Where a take ends up afterwards is save.c's question, and what is done to it
 * once it is on the timeline is actions.c's. This is only the recording of it:
 * opening the device, starting, draining, stopping, surviving, and rendering
 * the video that comes off the end.
 */
#include "gui/app.h"

#include "gui/engine.h"
#include "gui/render.h"

#include "backend/backend.h"
#include "backend/device.h"
#include "backend/monitor.h"
#include "edit/edit.h"
#include "edit/load.h"
#include "take/latency.h"
#include "take/take.h"
#include "util/log.h"
#include "util/path.h"

#include "raylib.h"

#include <stdio.h>
#include <string.h>

/* Open the device and build the display for whatever it negotiated. */
int app_open_engine(app *a)
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
  /* set again on every engine, so a device swap does not undo the level */
  aud_engine_set_input_gain(a->engine, a->input_gain);
  return 0;
}

void app_close_engine(app *a)
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
void app_recover_engine(app *a)
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

  /*
   * Loop turned on with a stretch selected means recording round that stretch:
   * the take starts at the top of it however far along the cursor is, the
   * transport goes round rather than running off the end, and every lap becomes
   * a pass of its own when the take stops.
   *
   * One recording and one file for all of it. The laps are cut out of what
   * arrived rather than being recorded separately - see aud_edit_take_passes()
   * - because tearing the device down and standing it up again at each lap
   * would lose the moment either side of every loop point.
   */
  a->lap_frames =
      a->loop && aud_doc_has_range(&a->doc) ? a->doc.sel_end - a->doc.sel_start : 0;
  at = a->lap_frames > 0 ? a->doc.sel_start : a->doc.cursor;

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

  if (along || a->click_on || a->lap_frames > 0)
  {
    /*
     * A loop take is bounded by the loop, whatever else is on. Otherwise the
     * project only when it was asked for, and the click runs either way and
     * past the end of what is there, so the pass has no end of its own.
     */
    uint64_t until = a->lap_frames > 0 ? a->doc.sel_end
                     : a->click_on     ? AUD_PLAYER_OPEN_ENDED
                                       : aud_doc_end(&a->doc);

    aud_player_set_mix(&a->player, along);
    app_apply_transport(a); /* which is what turns the loop on, see it for why */

    if (aud_player_start(&a->player, &a->doc, at, until, a->cfg.monitor_device) == 0)
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
    a->lap_frames = 0;
    app_set_status(a, "no room for another track");
    return;
  }

  if (aud_track_record_begin(&a->doc.tracks[target], start,
                             (size_t)aud_engine_rate(a->engine) * 8u) != 0)
  {
    aud_player_stop(&a->player);
    a->lap_frames = 0;
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
    a->lap_frames = 0;
    return;
  }

  if (a->lap_frames > 0)
  {
    app_set_status(a, "recording round %.2f s - every lap becomes a pass",
                   (double)a->lap_frames / aud_engine_rate(a->engine));
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
void app_pump_take(app *a)
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
      int passes = 1;

      snprintf(t->name, sizeof(t->name), "%s", aud_path_basename(take));

      /*
       * A take recorded round a loop, cut into one lane a lap. After the name,
       * because the passes take theirs from it - and only here, where the take
       * and the timeline are known to agree: the reload above rebuilds the lane
       * from the file at frame zero, which is not where the laps were.
       */
      if (a->lap_frames > 0)
      {
        passes = aud_edit_take_passes(&a->doc, (size_t)a->record_track, a->record_at,
                                      a->lap_frames);
      }

      if (passes > 1)
      {
        a->project_dirty = 1;
        app_set_status(a, "%d passes of %.2f s - K walks them, alt+K mutes one", passes,
                       (double)a->lap_frames / a->doc.rate);
      }
      else
      {
        app_set_status(a, "%.60s: %.1f s", aud_path_basename(take), seconds);
      }
    }
  }
  a->record_track = -1;
  a->record_skip = 0;
  a->lap_frames = 0;
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
      snprintf(a->interrupted.path, sizeof(a->interrupted.path), "%s", take);
    }
  }

  a->record_track = -1;
  a->record_skip = 0;
  /*
   * A loop take that was cut short is not cut into passes. What is on the lane
   * is however many laps got through before the cable went, and a second half
   * recorded when the device comes back starts wherever the first one stopped
   * rather than at the top of a lap - so the laps are no longer a fixed number
   * of frames apart and there is nothing honest to cut on. It stays one take.
   */
  a->lap_frames = 0;
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
void app_check_capture_loss(app *a)
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
 * The rest of the take goes on the end of the same file. Nothing already
 * written is rewritten to do it - the frames go after the ones that are there
 * and the header is patched when the take finally stops, so a crash part way
 * through leaves the file exactly as long as it was, which is the same amount
 * lost as a second file that never got created. See wav_open_append().
 *
 * A file that will not take them - one an editor has been at, or a device back
 * at another rate - falls back to what this used to always do, which is a
 * second take on the same lane. Two files is worse than one and much better
 * than losing the second half.
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

  t = &a->doc.tracks[a->interrupted.track];

  /*
   * The rest of the take on the end of the same file and the same clip, when
   * the file will take it. Both have to succeed together: one clip over two
   * files, or two clips over one file, would each be a lane that plays back
   * wrong once the project is saved and reloaded.
   */
  if (a->interrupted.path[0] != '\0' &&
      aud_track_record_continue(t, a->interrupted.at) == 0)
  {
    if (aud_engine_continue(a->engine, a->interrupted.path) == 0)
    {
      a->record_track = a->interrupted.track;
      a->record_at = a->interrupted.at;
      a->record_skip = 0;
      a->render_note[0] = '\0';
      app_set_status(a, "'%.30s' is back - carrying on in %.40s", a->active_device,
                     aud_path_basename(a->interrupted.path));
      return;
    }
    /* the file would not take it; put the clip back and fall through */
    aud_track_record_end(t);
  }

  /*
   * A second take on the same lane, which is what this always used to do. The
   * file could not be carried on - an editor has been at it, or the rate is
   * not what it was - and the half that is coming is worth more than the tidy
   * arrangement.
   */
  if (aud_take_next(path, sizeof(path), a->prefix) != 0)
  {
    return;
  }

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

  /* the block grew while it was being recorded into, so whatever the spectrum
   * panel read of it half way through is out of date */
  aud_repair_panel_reset(&a->repair);

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

void app_pump_render(app *a)
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
