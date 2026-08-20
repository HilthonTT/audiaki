/* SPDX-License-Identifier: MIT */
/*
 * actions.c - what the toolbar, the keys and the timeline all mean.
 *
 * Every one of these is something the window can be asked to do, and every one
 * of them can be asked for in more than one way: Cut is a button, a Ctrl+X and
 * an entry in the shortcut list, and there is exactly one place that knows what
 * it does and what it says afterwards. Having three would mean three answers to
 * "why did nothing happen?", drifting apart one fix at a time.
 *
 * app_cmd_run() at the bottom is the same idea one level up: keys.c decides
 * which of these a frame of the keyboard was asking for, and this carries it
 * out. Deciding and doing are apart so the deciding can be tested - see keys.h.
 *
 * Nothing here draws, and nothing here reads the keyboard.
 */
#include "gui/app.h"

#include "gui/keys.h"
#include "gui/player.h"
#include "gui/timeline.h"
#include "gui/viz.h"

#include "audio/limiter.h"
#include "edit/edit.h"
#include "edit/export.h"
#include "edit/limit.h"
#include "edit/load.h"
#include "util/path.h"

#include "raylib.h"

#include <stdio.h>

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
  aud_repair_panel_reset(&a->repair);

  app_set_status(a, "%.80s: %.1f s on track %d", aud_path_basename(path),
                 (double)aud_track_end(&a->doc.tracks[index]) / a->doc.rate, index + 1);
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
  /*
   * The one gate, so nothing can reach an edit without having been offered the
   * question - the toolbar, the keys and the timeline all come through here.
   * A question going up means nothing else happens this frame; the answer is
   * what calls app_edit_now().
   */
  if (action == APP_EDIT_UNDO)
  {
    if (app_confirm_undo(a))
    {
      return;
    }
  }
  else if (app_confirm_edit(a, action))
  {
    return;
  }

  app_edit_now(a, action);
}

/*
 * Move the selection by `by` frames, which is what a finished drag on the
 * timeline and the nudge keys both come to.
 *
 * The distance goes on the app rather than through app_edit(), because the
 * question a big move may put up is answered later and by then the drag that
 * asked for it has ended - see app.move_by.
 */
void app_move_selection(app *a, int64_t by)
{
  if (by == 0)
  {
    return;
  }

  a->move_by = by;
  app_edit(a, APP_EDIT_MOVE);
}

void app_edit_now(app *a, app_edit_action action)
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
  case APP_EDIT_MOVE:
  {
    /*
     * Its own reply rather than the shared one below, because what anyone
     * moving audio wants to be told is how far it went - the length of what
     * moved is the number they were already looking at. Asked for beforehand,
     * since a move that ran out of room went less far than it was asked to.
     */
    int64_t went = aud_edit_move_room(d, a->move_by);

    ok = aud_edit_move(d, a->move_by);
    a->move_by = 0;

    if (ok == 0)
    {
      a->project_dirty = 1;
      app_set_status(a, "moved %+.3f s", d->rate > 0 ? (double)went / d->rate : 0.0);
      return;
    }

    app_set_status(a, aud_doc_has_range(d) && aud_doc_any_track_selected(d)
                          ? "no room to move it that way"
                          : "select some audio first - click and drag across a track");
    return;
  }
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
  case APP_EDIT_LOUDER:
  case APP_EDIT_QUIETER:
  {
    double db = action == APP_EDIT_LOUDER ? APP_GAIN_STEP_DB : -APP_GAIN_STEP_DB;

    ok = aud_edit_gain(d, db);
    if (ok == 0)
    {
      /* the step, not the total: a clip's gain is per clip and a selection
       * across several of them has no single number to report */
      a->project_dirty = 1;
      app_set_status(a, "%+.1f dB", db);
      return;
    }
    break; /* and the shared "why it refused" below says why */
  }
  case APP_EDIT_NORMALIZE_PEAK:
  case APP_EDIT_NORMALIZE_LOUDNESS:
  {
    int loudness = action == APP_EDIT_NORMALIZE_LOUDNESS;
    double level = loudness ? AUD_NORMALIZE_LOUDNESS_DEFAULT : AUD_NORMALIZE_PEAK_DEFAULT;

    ok = aud_edit_normalize(d, loudness ? AUD_NORMALIZE_LOUDNESS : AUD_NORMALIZE_PEAK,
                            level);
    if (ok == 0)
    {
      a->project_dirty = 1;
      app_set_status(a, "normalized to %.1f %s", level, loudness ? "LUFS" : "dBTP");
      return;
    }

    /*
     * A normalize that refused with something selected refused for its own
     * reason - silence has no peak to raise, and BS.1770 has no loudness for a
     * selection under 400 ms - which the shared answer below would get wrong.
     */
    if (aud_doc_has_range(d) && aud_doc_any_track_selected(d))
    {
      app_set_status(a, loudness ? "too short or too quiet to measure a loudness"
                                 : "nothing to measure in that selection");
      return;
    }
    break;
  }
  case APP_EDIT_LIMIT:
  {
    double reduction = 0.0;
    const char *why = NULL;

    ok = aud_limit_selection(d, AUD_LIMITER_CEILING_DEFAULT, a->take_dir, &reduction,
                             &why);
    if (ok == 0)
    {
      a->project_dirty = 1;
      aud_repair_panel_reset(&a->repair); /* the audio under it is new audio */
      app_set_status(a, "limited by %.1f dB, to %.0f dBTP", reduction,
                     AUD_LIMITER_CEILING_DEFAULT);
      return;
    }

    /* it says why itself, and its reasons are better than the shared ones */
    if (why != NULL)
    {
      app_set_status(a, "%.90s", why);
      return;
    }
    break;
  }
  case APP_EDIT_MUTE_TOGGLE:
  {
    /*
     * Off what the first selected lane is doing, so the key is a toggle rather
     * than two keys: a selection that is being heard goes silent, and one that
     * has been silenced comes back.
     */
    int muted = 0;

    for (size_t i = 0; i < d->count; i++)
    {
      if (d->tracks[i].selected)
      {
        muted = aud_track_muted_at(&d->tracks[i], d->sel_start);
        break;
      }
    }

    ok = aud_edit_mute(d, !muted);
    if (ok == 0)
    {
      a->project_dirty = 1;
      app_set_status(a, muted ? "heard again" : "muted - alt+K brings it back");
      return;
    }
    break;
  }
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
  /*
   * The click counts out the grid the ruler draws, subdivision and all - on
   * bars it stays on the beat, because a metronome that only struck once a bar
   * is not one anybody could play to.
   */
  aud_player_set_click(&a->player, a->click_on ? a->doc.tempo : 0.0, a->doc.beats_per_bar,
                       a->doc.grid_div < AUD_DOC_GRID_BEAT ? AUD_DOC_GRID_BEAT
                                                           : a->doc.grid_div,
                       a->click_gain);
  /*
   * While a take is open, only for the take that was started against a loop.
   * That one is meant to go round - each lap becomes a pass of its own when it
   * stops, see aud_edit_take_passes() - where a straight take laid over music
   * that repeated underneath it would be nobody's intention.
   */
  aud_player_set_loop(&a->player, a->loop && (a->record_track < 0 || a->lap_frames > 0));
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

/* What both of the exports below are asked for, which is the same request. */
static void app_export_options(const app *a, const char *path, aud_export_options *opts)
{
  aud_export_defaults(opts);
  opts->path = path;
  opts->overwrite = 1; /* the browser already asked, and said so if it existed */

  /*
   * The selection when there is one. Exporting a chorus you have just cut down
   * to should not mean exporting the whole session and cutting it again in
   * something else.
   */
  if (aud_doc_has_range(&a->doc))
  {
    opts->from = a->doc.sel_start;
    opts->to = a->doc.sel_end;
  }
}

void app_export(app *a, const char *path)
{
  aud_export_options opts;
  const char *why = NULL;

  app_export_options(a, path, &opts);

  if (aud_export_wav(&a->doc, &opts, &why) != 0)
  {
    app_set_status(a, "cannot export: %s", why != NULL ? why : "unknown");
    return;
  }

  app_set_status(a, "wrote %.80s", aud_path_basename(path));
}

void app_export_stems(app *a, const char *path)
{
  aud_export_options opts;
  const char *why = NULL;
  size_t written = 0;

  app_export_options(a, path, &opts);

  if (aud_export_stems(&a->doc, &opts, &written, &why) != 0)
  {
    app_set_status(a, "cannot export stems: %s", why != NULL ? why : "unknown");
    return;
  }

  /*
   * The count rather than the names: there are as many as there are lanes, and
   * they are all named after what was typed into the dialog a moment ago.
   */
  app_set_status(a, "wrote %zu stem(s) beside %.60s", written, aud_path_basename(path));
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

  /* the width the waveform actually got at the last draw; see timeline.h */
  aud_timeline_reveal(&a->timeline, &a->doc, to, a->timeline.wave_w);
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

/* -- the ruler, and choosing between passes -------------------------------- */

/* Frames either side of the cursor that count as "on" a marker at this zoom. */
static uint64_t marker_reach(const app *a)
{
  if (!(a->timeline.zoom > 0.0) || a->doc.rate == 0)
  {
    return 0;
  }
  return (uint64_t)((double)a->doc.rate * APP_MARKER_GRAB_PX / a->timeline.zoom);
}

/*
 * Drop a marker where the cursor is, or take away the one already there.
 *
 * "Already there" is a few pixels' worth of time rather than the exact frame:
 * the cursor lands where it was clicked or where the grid put it, and a key
 * that only removed a marker sitting on precisely the same sample would be a
 * key that never removed one.
 */
void app_mark(app *a)
{
  uint64_t at = a->doc.cursor;
  long found = aud_doc_marker_near(&a->doc, at, marker_reach(a));

  if (found >= 0)
  {
    aud_doc_unmark(&a->doc, (size_t)found);
    a->project_dirty = 1;
    app_set_status(a, "marker removed");
    return;
  }

  if (aud_doc_mark(&a->doc, at, "") < 0)
  {
    app_set_status(a, "that project already holds as many markers as one can");
    return;
  }

  a->project_dirty = 1;
  app_set_status(a, "marked at %.2f s - ctrl+arrow steps to it",
                 a->doc.rate > 0 ? (double)at / a->doc.rate : 0.0);
}

/*
 * Comp: walk which of the selected lanes is heard over the selection.
 *
 * One press a lane, so four passes of a bar are auditioned by pressing the same
 * key four times rather than by reaching for four different lanes. Where it
 * starts from is whichever lane is being heard now - which after a loop take is
 * the last pass, and after an earlier press is whatever that press chose.
 */
void app_comp(app *a, int forward)
{
  aud_doc *d = &a->doc;
  size_t lanes[AUD_DOC_MAX_TRACKS];
  size_t count = 0;
  size_t at = 0;

  if (!aud_doc_has_range(d))
  {
    app_set_status(a, "select the stretch to comp first");
    return;
  }

  for (size_t i = 0; i < d->count && count < AUD_DOC_MAX_TRACKS; i++)
  {
    if (d->tracks[i].selected)
    {
      lanes[count++] = i;
    }
  }

  if (count < 2u)
  {
    app_set_status(a,
                   "comping needs two lanes or more - select the passes to choose from");
    return;
  }

  /* the lane being heard now, or the first one when none of them is */
  for (size_t i = 0; i < count; i++)
  {
    if (!aud_track_muted_at(&d->tracks[lanes[i]], d->sel_start))
    {
      at = i;
      break;
    }
  }

  at = forward ? (at + 1u) % count : (at + count - 1u) % count;

  if (aud_edit_comp(d, lanes[at]) != 0)
  {
    app_set_status(a, "there is nothing on those lanes to choose between");
    return;
  }

  a->project_dirty = 1;
  app_set_status(a, "%.40s (%zu of %zu)", d->tracks[lanes[at]].name, at + 1u, count);
}

/* -- the keyboard's commands ----------------------------------------------- */

/*
 * Carry one out.
 *
 * A plain switch over every command there is, and deliberately without a
 * default that swallows the ones it does not know: adding a command to keys.h
 * and forgetting to say what it does should be a warning at the compiler rather
 * than a key that quietly does nothing.
 */
void app_cmd_run(app *a, const app_cmd *cmd, const aud_engine_status *st)
{
  int shift = (cmd->mod & APP_MOD_SHIFT) != 0;

  switch (cmd->kind)
  {
  case APP_CMD_HELP_TOGGLE:
    a->help_open = !a->help_open;
    return;

  case APP_CMD_HELP_CLOSE:
    a->help_open = 0;
    return;

  case APP_CMD_MENU_CLOSE:
    a->device_menu_open = 0;
    return;

  case APP_CMD_EDIT:
    app_edit(a, (app_edit_action)cmd->arg);
    return;

  case APP_CMD_MARK:
    app_mark(a);
    return;

  case APP_CMD_COMP:
    app_comp(a, cmd->arg > 0);
    return;

  case APP_CMD_CURSOR_MOVE:
    app_move_cursor(a,
                    app_cursor_target(a, cmd->arg < 0, (cmd->mod & APP_MOD_CTRL) != 0,
                                      (cmd->mod & APP_MOD_ALT) != 0, shift),
                    shift);
    return;

  case APP_CMD_CURSOR_HOME:
    app_move_cursor(a, 0, shift);
    return;

  case APP_CMD_CURSOR_END:
    app_move_cursor(a, aud_doc_end(&a->doc), shift);
    return;

  case APP_CMD_STEP_TRACK:
    app_step_track(a, cmd->arg > 0, shift);
    return;

  case APP_CMD_MOVE_SELECTION:
    app_move_selection(a, app_move_step(a, cmd->arg < 0, (cmd->mod & APP_MOD_ALT) != 0));
    return;

  case APP_CMD_TOGGLE_PLAY:
    app_toggle_play(a);
    return;

  case APP_CMD_TOGGLE_RECORD:
    app_toggle_record(a, st);
    return;

  /*
   * Whichever of the three is running, in the order that loses the least: a
   * render is only ever worth abandoning, a take has a file to close, and
   * playback costs nothing to stop and start again.
   */
  case APP_CMD_STOP:
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
    return;

  case APP_CMD_TOGGLE_MONITOR:
    aud_engine_set_monitor(a->engine, !aud_engine_monitor_wanted(a->engine));
    return;

  case APP_CMD_TOGGLE_LOOP:
    a->loop = !a->loop;
    app_apply_transport(a);
    app_set_status(a, "%s", a->loop ? "looping" : "playing straight through");
    return;

  case APP_CMD_TOGGLE_CLICK:
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
    return;

  case APP_CMD_TOGGLE_GRID:
    a->timeline.grid = !a->timeline.grid;
    app_set_status(a, "%s",
                   a->timeline.grid ? "grid on - shift+G divides it, alt steps off it"
                                    : "grid off");
    return;

  case APP_CMD_CYCLE_GRID:
  {
    static const unsigned steps[] = {AUD_DOC_GRID_BAR, AUD_DOC_GRID_BEAT, 2u, 3u, 4u};
    size_t at = 0;

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
    {
      if (steps[i] == a->doc.grid_div)
      {
        at = i + 1u;
        break;
      }
    }
    aud_doc_set_grid(&a->doc, steps[at % (sizeof(steps) / sizeof(steps[0]))]);
    a->timeline.grid = 1;
    /* the click counts the grid, so a division it has not been told about
     * would leave the ruler and the headphones disagreeing until the next
     * time something else re-armed it */
    app_apply_transport(a);
    app_set_status(a, "grid: %s", aud_doc_grid_label(&a->doc));
    return;
  }

  case APP_CMD_NUDGE_TEMPO:
    app_nudge_tempo(a, (double)cmd->arg);
    return;

  case APP_CMD_DRAWER_VIZ:
    a->drawer = a->drawer == APP_DRAWER_VIZ ? APP_DRAWER_NONE : APP_DRAWER_VIZ;
    return;

  case APP_CMD_DRAWER_SPECTRUM:
    a->drawer = a->drawer == APP_DRAWER_SPECTRUM ? APP_DRAWER_NONE : APP_DRAWER_SPECTRUM;
    /* it reads the timeline rather than the interface, so opening it is what
     * asks for a reading at all - see gui/repair.h */
    aud_repair_panel_reset(&a->repair);
    return;

  case APP_CMD_CYCLE_STYLE:
    a->style_selected = (int)aud_viz_cycle_mode(a->viz);
    return;

  case APP_CMD_SET_STYLE:
    a->style_selected = (int)cmd->arg;
    aud_viz_set_mode(a->viz, (aud_viz_mode)cmd->arg);
    return;

  /* the width the lanes actually got at the last draw, which only it knows */
  case APP_CMD_FIT:
    if (aud_doc_has_range(&a->doc))
    {
      aud_timeline_fit_selection(&a->timeline, &a->doc, a->timeline.wave_w);
    }
    else
    {
      aud_timeline_fit(&a->timeline, &a->doc, a->timeline.wave_w);
    }
    return;

  case APP_CMD_FULLSCREEN:
    ToggleFullscreen();
    return;

  case APP_CMD_OPEN_DIALOG:
    app_open_dialog(a);
    return;

  case APP_CMD_EXPORT_DIALOG:
    app_export_dialog(a, shift);
    return;

  case APP_CMD_PROJECT_SAVE:
    app_save_project(a);
    return;

  case APP_CMD_PROJECT_SAVE_AS:
    app_save_project_as(a);
    return;

  case APP_CMD_PROJECT_OPEN:
    app_open_project_dialog(a);
    return;

  case APP_CMD_NONE:
  case APP_CMD_COUNT:
    return;
  }
}
