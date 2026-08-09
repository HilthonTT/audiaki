/* SPDX-License-Identifier: MIT */
/*
 * screen.c - the chrome around the tracks.
 *
 * Apart from main.c because it is the half that changes when the window is
 * redesigned rather than when the recorder is: it reads `app` and the engine's
 * status, and the only writing it does is through the transport and edit
 * actions the buttons stand for. The tracks themselves are timeline.c, which is
 * the same arrangement one level down. See app.h.
 *
 * The layout, top to bottom: the title and the device picker, a transport bar,
 * an edit bar, the visualiser panel (shut by default to nothing but its own
 * name), the time ruler, the tracks, and a status bar. It is the arrangement
 * every editor of this kind has, for the reason they all have it - the controls
 * are where the hand already is, and the tracks get everything left over.
 */
#include "gui/app.h"

#include "gui/timeline.h"
#include "gui/ui.h"
#include "gui/viz.h"

#include "audio/format.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Buttons in a toolbar row: wide enough to read, narrow enough to all fit. */
#define SCREEN_BUTTON_MIN 46.0f
#define SCREEN_BUTTON_MAX 96.0f
#define SCREEN_BUTTON_GAP 5.0f

/* Loop, which shares the transport row without taking a whole slot of it. */
#define SCREEN_LOOP_W 52.0f

/*
 * The tempo cluster at the end of the edit bar: the metronome, the number it
 * counts on and the grid it draws. One group because they are one idea - what
 * a bar is - and separating them would mean hunting for the tempo in one row
 * and the thing it drives in another.
 */
#define SCREEN_CLICK_W 52.0f
#define SCREEN_STEP_W 26.0f
#define SCREEN_TEMPO_W 60.0f
#define SCREEN_GRID_W 46.0f
#define SCREEN_ZOOM_W 34.0f

/* The device dropdown's slot, which the header leaves clear for it. */
static Rectangle header_picker(Rectangle r)
{
  Rectangle picker;

  picker.width = 260.0f;
  if (picker.width > r.width * 0.4f)
  {
    picker.width = r.width * 0.4f;
  }
  picker.height = 30.0f;
  picker.x = r.x + r.width - picker.width;
  picker.y = r.y + (r.height - picker.height) / 2.0f;
  return picker;
}

/* The shortcut list's button, tucked in beside the picker. */
static Rectangle header_help(Rectangle r)
{
  Rectangle picker = header_picker(r);
  Rectangle help;

  help.width = 28.0f;
  help.height = 28.0f;
  help.x = picker.x - 10.0f - help.width;
  help.y = picker.y + (picker.height - help.height) / 2.0f;
  return help;
}

/* Non-zero when something is over the window and nothing beneath may be used. */
static int covered(const app *a)
{
  return a->device_menu_open || a->help_open || a->save.open;
}

/*
 * Hover help, suppressed while something is over the top: a tooltip for a
 * control the user cannot currently reach is noise.
 */
static void tip(const app *a, Rectangle bounds, const char *text)
{
  if (covered(a))
  {
    return;
  }

  aud_ui_tooltip(bounds, text);
}

static void draw_header(app *a, Rectangle r)
{
  Rectangle help = header_help(r);
  char detail[128];

  aud_ui_text(r.x, r.y + 6.0f, 26, AUD_UI_TEXT, AUDIAKI_NAME);

  /*
   * The keys are the fast way to drive this and they are invisible, so there is
   * something to click that says what they are.
   */
  if (aud_ui_toggle(help, "?", a->help_open, AUD_UI_ACCENT,
                    !a->device_menu_open && !a->save.open))
  {
    a->help_open = !a->help_open;
  }
  tip(a, help, "keyboard shortcuts   ?");

  /* what the device negotiated, immediately left of the picker that chose it */
  if (a->engine != NULL)
  {
    snprintf(detail, sizeof(detail), "%u Hz   %u ch   %s", aud_engine_rate(a->engine),
             aud_engine_channels(a->engine),
             aud_format_name(aud_engine_format(a->engine)));
    aud_ui_text_right(help.x - 16.0f, r.y + 12.0f, 16, AUD_UI_MUTED, detail);
  }
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

/* The monitoring gain in the units it is thought about in. */
static void format_monitor_gain(char *dst, size_t size, float gain)
{
  if (gain <= 0.001f)
  {
    snprintf(dst, size, "silent");
    return;
  }

  snprintf(dst, size, "%+.1f dB", 20.0 * log10((double)gain));
}

/*
 * Hand out `count` equal slots across `row`, left to right, and return the
 * width of one. A toolbar that overflowed at the minimum window width would be
 * a toolbar with a button nobody can reach, so they shrink rather than spill.
 */
static float slot_width(Rectangle row, int count)
{
  float w;

  if (count <= 0)
  {
    return 0.0f;
  }

  w = (row.width - SCREEN_BUTTON_GAP * (float)(count - 1)) / (float)count;
  if (w > SCREEN_BUTTON_MAX)
  {
    w = SCREEN_BUTTON_MAX;
  }
  if (w < SCREEN_BUTTON_MIN)
  {
    w = SCREEN_BUTTON_MIN;
  }
  return w;
}

static Rectangle slot_at(Rectangle row, float w, int index)
{
  Rectangle r = row;

  r.x = row.x + (w + SCREEN_BUTTON_GAP) * (float)index;
  r.width = w;
  return r;
}

/* -- the transport bar ------------------------------------------------------ */

static void draw_transport(app *a, Rectangle r, const aud_engine_status *st)
{
  int recording = st->state == AUD_ENGINE_RECORDING;
  int paused = st->state == AUD_ENGINE_PAUSED;
  int rendering = a->render != NULL;
  int live = (recording || paused) && !covered(a);
  int usable = a->engine != NULL && st->state != AUD_ENGINE_FAILED && !covered(a);
  int playing = aud_player_playing(&a->player);
  float w = slot_width(r, 11);
  Rectangle play = slot_at(r, w, 0);
  /*
   * Loop takes a slot of its own rather than a full one: it is a modifier on
   * Play rather than a transport button in its own right, and giving it the
   * same width as Record would say otherwise - as well as pushing the capture
   * options off the end of the bar on a narrower window.
   */
  Rectangle loop = {play.x + play.width + SCREEN_BUTTON_GAP, r.y, SCREEN_LOOP_W,
                    r.height};
  Rectangle rest = r;
  Rectangle rec;
  Rectangle pause;
  Rectangle stop;
  Rectangle import;
  Rectangle export_to;
  Rectangle open_project;
  Rectangle save_project;

  rest.x = loop.x + loop.width + SCREEN_BUTTON_GAP;
  rest.width = r.x + r.width - rest.x;

  rec = slot_at(rest, w, 0);
  pause = slot_at(rest, w, 1);
  stop = slot_at(rest, w, 2);
  import = slot_at(rest, w, 3);
  export_to = slot_at(rest, w, 4);
  open_project = slot_at(rest, w, 5);
  save_project = slot_at(rest, w, 6);

  /*
   * Play first, because it is the one pressed most and the one the eye goes to
   * first. Recording is what the window was for; playing back is what it is for
   * once there is something on the timeline.
   */
  /* the metronome is something to play even with an empty timeline, and
   * counting a bar in before the first take is exactly what it is for */
  if (aud_ui_button(play, playing ? "Playing" : "Play", AUD_UI_OK,
                    !covered(a) && !live && (a->doc.count > 0 || a->click_on)))
  {
    app_toggle_play(a);
  }
  tip(a, play,
      (a->doc.count == 0 && !a->click_on)
          ? "nothing on the timeline to play - turn Click on to count instead"
          : (live ? "stop the take first"
                  : (playing ? "stop playing   space"
                             : "play the selection, or from the cursor   space")));

  /*
   * Beside Play because it is a way of playing rather than a thing of its
   * own: what it changes is what happens when the passage being played runs
   * out. Settable while it is running, so a passage can be put on repeat
   * without stopping it first.
   */
  if (aud_ui_toggle(loop, "Loop", a->loop, AUD_UI_OK, !covered(a) && !live))
  {
    a->loop = !a->loop;
    app_apply_transport(a);
  }
  tip(a, loop,
      live ? "stop the take first"
           : (aud_doc_has_range(&a->doc)
                  ? "play the selection round and round   L"
                  : "play round and round; select a passage to loop that   L"));

  /* a render holds the drawing thread, so no new take can start under it */
  if (aud_ui_button(rec, live ? "Recording" : "Record", AUD_UI_RECORD,
                    usable && !live && !rendering))
  {
    app_begin_take(a);
  }

  if (rendering)
  {
    tip(a, rec, "the video is still being written");
  }
  else if (recording || paused)
  {
    tip(a, rec, "a take is already open - stop it first");
  }
  else if (!usable)
  {
    tip(a, rec, "no capture device");
  }
  else
  {
    tip(a, rec, "record from the cursor onto the timeline   R");
  }

  if (aud_ui_button(pause, paused ? "Resume" : "Pause", AUD_UI_WARN, live))
  {
    if (paused)
    {
      aud_engine_resume(a->engine);
    }
    else
    {
      aud_engine_pause(a->engine);
    }
  }
  tip(a, pause,
      paused ? "carry on writing to the same file   space"
             : (recording ? "stop writing without closing the file   space"
                          : "nothing to pause - no take is open"));

  /*
   * The same slot stops the take and, once the take is stopped and its video
   * is being written, abandons that. Both are "I have had enough of this".
   */
  if (rendering)
  {
    if (aud_ui_button(stop, "Cancel", AUD_UI_WARN, !covered(a)))
    {
      app_cancel_render(a);
    }
    tip(a, stop, "drop the part-written video and keep the take   S");
  }
  else
  {
    if (aud_ui_button(stop, "Stop", AUD_UI_ACCENT, live))
    {
      app_stop_take(a, st);
    }
    tip(a, stop,
        (recording || paused) ? "close the take and put it on the timeline   S"
                              : "nothing to stop - no take is open");
  }

  if (aud_ui_button(import, "Import", AUD_UI_ACCENT, !covered(a) && !live))
  {
    app_open_dialog(a);
  }
  tip(a, import, live ? "stop the take first" : "open a WAV as a new track   I");

  if (aud_ui_button(export_to, "Export", AUD_UI_ACCENT,
                    !covered(a) && !live && a->doc.count > 0))
  {
    app_export_dialog(a);
  }
  tip(a, export_to,
      a->doc.count == 0 ? "nothing to export yet"
                        : "mix the project down to a WAV   ctrl+E");

  /*
   * The session itself, next to Export because both are about what leaves the
   * window - but these two write the edits rather than the audio, and the
   * tooltips are where that distinction is made.
   */
  if (aud_ui_button(open_project, "Open", AUD_UI_ACCENT, !covered(a) && !live))
  {
    app_open_project_dialog(a);
  }
  tip(a, open_project, live ? "stop the take first" : "open a saved session   ctrl+O");

  if (aud_ui_button(save_project, a->project_dirty ? "Save *" : "Save", AUD_UI_ACCENT,
                    !covered(a) && a->doc.count > 0))
  {
    app_save_project(a);
  }
  tip(a, save_project,
      a->doc.count == 0
          ? "nothing to save yet"
          : "save the session - the tracks and edits, not the audio   ctrl+S");

  /* the capture options sit at the right hand end, away from the transport */
  {
    float slider_w = 120.0f;
    float ctl_w = 92.0f;
    Rectangle slider = {r.x + r.width - slider_w, r.y + (r.height - 22.0f) / 2.0f,
                        slider_w, 22.0f};
    Rectangle monitor = {slider.x - SCREEN_BUTTON_GAP - ctl_w, r.y, ctl_w, r.height};
    Rectangle audio = {monitor.x - SCREEN_BUTTON_GAP - ctl_w, r.y, ctl_w, r.height};
    Rectangle video = {audio.x - SCREEN_BUTTON_GAP - ctl_w, r.y, ctl_w, r.height};
    Rectangle overdub = {video.x - SCREEN_BUTTON_GAP - ctl_w, r.y, ctl_w, r.height};
    int wanted = a->engine != NULL && aud_engine_monitor_wanted(a->engine);
    /*
     * Only settable between takes: the video is rendered from the finished
     * WAV, so changing your mind halfway through would be answered either by
     * rendering the whole take or none of it, and neither is what the click
     * meant.
     */
    int settable = usable && !live && !rendering;

    if (overdub.x < save_project.x + save_project.width + SCREEN_BUTTON_GAP)
    {
      return; /* too narrow a window for these; the transport comes first */
    }

    /*
     * Playing the project while recording over it. Only settable between
     * takes, like the video options: it decides what the transport does when
     * Record is pressed, and changing it mid-take would answer a question that
     * has already been answered.
     */
    if (aud_ui_toggle(overdub, "Overdub", a->overdub, AUD_UI_OK, settable))
    {
      a->overdub = !a->overdub;
    }
    tip(a, overdub,
        a->doc.count == 0
            ? "nothing on the timeline to play along to yet"
            : (settable ? "play the project while recording over it - use headphones"
                        : "only settable between takes"));

    if (aud_ui_toggle(video, "Video", a->want_video, AUD_UI_ACCENT, settable))
    {
      a->want_video = !a->want_video;
    }
    tip(a, video,
        settable ? "also render an MP4 of the visualiser when the take stops"
                 : "only settable between takes");

    if (aud_ui_toggle(audio, a->want_video_audio ? "Audio" : "No audio",
                      a->want_video_audio, AUD_UI_ACCENT, settable && a->want_video))
    {
      a->want_video_audio = !a->want_video_audio;
    }
    tip(a, audio,
        !a->want_video ? "turn Video on first"
                       : (settable ? "whether that video carries the take's own audio"
                                   : "only settable between takes"));

    if (aud_ui_toggle(monitor, st->monitoring ? "Monitor on" : "Monitor", wanted,
                      AUD_UI_OK, usable))
    {
      aud_engine_set_monitor(a->engine, !wanted);
    }
    tip(a, monitor, "hear the input through the default output   M");

    if (aud_ui_slider(slider, &a->monitor_gain, 0.0f, 2.0f, AUD_UI_OK, usable))
    {
      aud_engine_set_monitor_gain(a->engine, a->monitor_gain);
    }
    tip(a, slider, "monitoring level, silent to +6 dB - the wheel nudges it");
  }
}

/* -- the edit bar ----------------------------------------------------------- */

/* Every button on it, in order, with what each is for. */
static const struct
{
  const char *label;
  app_edit_action action;
  const char *tip;
} screen_edits[] = {
    {"Undo", APP_EDIT_UNDO, "take back the last edit   ctrl+Z"},
    {"Redo", APP_EDIT_REDO, "put it back   ctrl+shift+Z"},
    {"Cut", APP_EDIT_CUT, "remove the selection and keep it   ctrl+X"},
    {"Copy", APP_EDIT_COPY, "keep the selection   ctrl+C"},
    {"Paste", APP_EDIT_PASTE, "drop the clipboard in at the cursor   ctrl+V"},
    {"Delete", APP_EDIT_DELETE, "remove the selection and close the gap   del"},
    {"Silence", APP_EDIT_SILENCE, "empty the selection, leaving the timing alone"},
    {"Trim", APP_EDIT_TRIM, "throw away everything outside the selection"},
    {"Split", APP_EDIT_SPLIT, "cut the clips at the edges of the selection"},
    {"Copy to", APP_EDIT_DUPLICATE, "the selection onto a new track of its own"},
    {"Fade in", APP_EDIT_FADE_IN, "ramp the selection up out of silence   ["},
    {"Fade out", APP_EDIT_FADE_OUT, "ramp the selection down into silence   ]"},
};

#define SCREEN_EDIT_COUNT ((int)(sizeof(screen_edits) / sizeof(screen_edits[0])))

/*
 * The metronome, the tempo and the grid, drawn as one control at the end of
 * the edit bar. The reading between the two steps is text rather than a field:
 * a tempo is arrived at by nudging it against what you are playing until it
 * sits right, not by typing a number you already knew.
 */
static void draw_tempo_cluster(app *a, Rectangle click, Rectangle slower,
                               Rectangle reading, Rectangle faster, Rectangle grid)
{
  int usable = !covered(a);
  int shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  double step = shift ? 10.0 : 1.0;
  char text[32];

  if (aud_ui_toggle(click, "Click", a->click_on, AUD_UI_WARN, usable))
  {
    a->click_on = !a->click_on;
    app_apply_transport(a);
  }
  tip(a, click,
      a->click_on ? "the metronome is playing - it is heard, never recorded   C"
                  : "play a metronome over whatever is being heard   C");

  if (aud_ui_button(slower, "-", AUD_UI_ACCENT, usable))
  {
    app_nudge_tempo(a, -step);
  }
  tip(a, slower, "slower; hold shift for ten at a time   -");

  DrawRectangleRec(reading, AUD_UI_PANEL);
  snprintf(text, sizeof(text), "%.0f BPM", a->doc.tempo);
  aud_ui_text_centred(reading, 15, a->click_on ? AUD_UI_TEXT : AUD_UI_MUTED, text);
  tip(a, reading, "the tempo this session is counted on; it is saved with it");

  if (aud_ui_button(faster, "+", AUD_UI_ACCENT, usable))
  {
    app_nudge_tempo(a, step);
  }
  tip(a, faster, "faster; hold shift for ten at a time   +");

  if (aud_ui_toggle(grid, "Grid", a->timeline.grid, AUD_UI_ACCENT, usable))
  {
    a->timeline.grid = !a->timeline.grid;
  }
  tip(a, grid,
      a->timeline.grid ? "the ruler is counting bars; alt drops off the grid   G"
                       : "count the ruler in bars, and put the pointer on them   G");
}

static void draw_edit_bar(app *a, Rectangle r, Rectangle wave_area)
{
  int usable = !covered(a);
  Rectangle zoom_out = {r.x + r.width - SCREEN_ZOOM_W * 3.0f - SCREEN_BUTTON_GAP * 2.0f,
                        r.y, SCREEN_ZOOM_W, r.height};
  Rectangle zoom_in = {zoom_out.x + SCREEN_ZOOM_W + SCREEN_BUTTON_GAP, r.y, SCREEN_ZOOM_W,
                       r.height};
  Rectangle fit = {zoom_in.x + SCREEN_ZOOM_W + SCREEN_BUTTON_GAP, r.y, SCREEN_ZOOM_W,
                   r.height};
  Rectangle grid = {zoom_out.x - SCREEN_BUTTON_GAP - SCREEN_GRID_W, r.y, SCREEN_GRID_W,
                    r.height};
  /* the three of these butt up against each other: one control, not three */
  Rectangle faster = {grid.x - SCREEN_BUTTON_GAP - SCREEN_STEP_W, r.y, SCREEN_STEP_W,
                      r.height};
  Rectangle reading = {faster.x - SCREEN_TEMPO_W, r.y, SCREEN_TEMPO_W, r.height};
  Rectangle slower = {reading.x - SCREEN_STEP_W, r.y, SCREEN_STEP_W, r.height};
  Rectangle click = {slower.x - SCREEN_BUTTON_GAP - SCREEN_CLICK_W, r.y, SCREEN_CLICK_W,
                     r.height};
  /*
   * What is left for the edits, once the clusters at the far end have had
   * theirs. Sized from the room that remains rather than from a slot count
   * that pretends they are buttons in the same row: a fixed group and a
   * sharing one do not divide the same way.
   */
  Rectangle row = r;
  float w;

  row.width = click.x - SCREEN_BUTTON_GAP - row.x;
  if (row.width < SCREEN_BUTTON_MIN)
  {
    row.width = SCREEN_BUTTON_MIN;
  }
  w = slot_width(row, SCREEN_EDIT_COUNT);

  for (int i = 0; i < SCREEN_EDIT_COUNT; i++)
  {
    Rectangle slot = slot_at(row, w, i);
    int enabled = usable;

    if (slot.x + slot.width > row.x + row.width)
    {
      break; /* the window is too narrow for the rest; zoom stays reachable */
    }

    /*
     * Greyed out when they would refuse, so the toolbar answers "why is
     * nothing happening?" before it is asked. Undo and redo know their own
     * answer; the rest need a selection to work on.
     */
    if (screen_edits[i].action == APP_EDIT_UNDO)
    {
      enabled = usable && aud_doc_undo_label(&a->doc) != NULL;
    }
    else if (screen_edits[i].action == APP_EDIT_REDO)
    {
      enabled = usable && aud_doc_redo_label(&a->doc) != NULL;
    }
    else if (screen_edits[i].action == APP_EDIT_PASTE)
    {
      enabled = usable && !aud_clipboard_empty(&a->clipboard);
    }
    else
    {
      enabled =
          usable && aud_doc_has_range(&a->doc) && aud_doc_any_track_selected(&a->doc);
    }

    if (aud_ui_button(slot, screen_edits[i].label, AUD_UI_ACCENT, enabled))
    {
      app_edit(a, screen_edits[i].action);
    }
    tip(a, slot, enabled ? screen_edits[i].tip : "select some audio on a track first");
  }

  draw_tempo_cluster(a, click, slower, reading, faster, grid);

  if (aud_ui_button(zoom_out, "-", AUD_UI_ACCENT, usable))
  {
    aud_timeline_zoom_at(&a->timeline, 1.0 / AUD_TIMELINE_ZOOM_STEP, a->timeline.scroll,
                         wave_area.width);
  }
  tip(a, zoom_out, "zoom out   ctrl+wheel over the tracks");

  if (aud_ui_button(zoom_in, "+", AUD_UI_ACCENT, usable))
  {
    aud_timeline_zoom_at(&a->timeline, AUD_TIMELINE_ZOOM_STEP, a->timeline.scroll,
                         wave_area.width);
  }
  tip(a, zoom_in, "zoom in   ctrl+wheel over the tracks");

  if (aud_ui_button(fit, "[ ]", AUD_UI_ACCENT, usable))
  {
    if (aud_doc_has_range(&a->doc))
    {
      aud_timeline_fit_selection(&a->timeline, &a->doc, wave_area.width);
    }
    else
    {
      aud_timeline_fit(&a->timeline, &a->doc, wave_area.width);
    }
  }
  tip(a, fit, "fit the selection, or the whole project   F");
}

/* -- the visualiser panel --------------------------------------------------- */

/*
 * The visualiser, which used to be the window and is now a drawer in it.
 *
 * Shut it and the tracks get the room; open it and the meters are back. It
 * keeps running either way - see app.h - so opening it shows what is happening
 * now rather than an empty panel filling up.
 */
static void draw_viz_panel(app *a, Rectangle r)
{
  Rectangle bar = r;
  Rectangle toggle;

  bar.height = APP_VIZ_BAR_H;

  DrawRectangleRec(bar, AUD_UI_PANEL);

  toggle.x = bar.x;
  toggle.y = bar.y;
  toggle.width = 130.0f;
  toggle.height = bar.height;

  if (aud_ui_toggle(toggle, a->viz_open ? "v  Visualiser" : ">  Visualiser", a->viz_open,
                    AUD_UI_ACCENT, !covered(a)))
  {
    a->viz_open = !a->viz_open;
  }
  tip(a, toggle, "show or hide the live visualiser   B");

  if (!a->viz_open || a->viz == NULL)
  {
    return;
  }

  {
    Rectangle stage = {r.x, bar.y + bar.height, r.width, r.height - bar.height};
    Rectangle inner = {stage.x + 8.0f, stage.y + 6.0f, stage.width - 16.0f,
                       stage.height - 12.0f};

    if (inner.width < 40.0f || inner.height < 30.0f)
    {
      return;
    }

    /* nearly black so the additive glow has somewhere to go */
    DrawRectangleRec(stage, BLACK);
    DrawRectangleLinesEx(stage, 1.0f, AUD_UI_EDGE);
    aud_viz_draw(a->viz, inner);

    if (stage.width > 380.0f && stage.height > 60.0f)
    {
      float tabs_w = 88.0f * (float)AUD_VIZ_MODE_COUNT;
      Rectangle tabs;

      if (tabs_w > stage.width - 20.0f)
      {
        tabs_w = stage.width - 20.0f;
      }

      tabs.x = stage.x + stage.width - 10.0f - tabs_w;
      tabs.y = stage.y + 8.0f;
      tabs.width = tabs_w;
      tabs.height = 22.0f;

      if (aud_ui_tabs(tabs, a->style_labels, AUD_VIZ_MODE_COUNT, &a->style_selected,
                      !covered(a), 1))
      {
        aud_viz_set_mode(a->viz, (aud_viz_mode)a->style_selected);
      }
      tip(a, tabs, "visualiser style   V, or 1 - 6");
    }
  }
}

/* -- the status bar --------------------------------------------------------- */

/* A position on the timeline, in the units the ruler is labelled in. */
static void format_position(char *dst, size_t size, uint64_t frame, unsigned rate)
{
  double seconds = rate > 0 ? (double)frame / rate : 0.0;
  unsigned m = (unsigned)(seconds / 60.0);

  snprintf(dst, size, "%u:%06.3f", m, seconds - (double)m * 60.0);
}

static void draw_status(const app *a, Rectangle r, const aud_engine_status *st)
{
  char clock[16];
  char text[320];
  float meter_w = 190.0f;
  Rectangle meter;
  float x = r.x;
  float top = r.y + 2.0f;

  DrawLine(0, (int)r.y, GetScreenWidth(), (int)r.y, AUD_UI_EDGE);

  draw_record_light((Rectangle){r.x, top, 16.0f, 20.0f}, st->state, GetTime());
  x += 24.0f;

  aud_ui_format_clock(clock, sizeof(clock), st->elapsed);
  aud_ui_text(x, top, 22, AUD_UI_TEXT, clock);
  x += (float)MeasureText(clock, 22) + 16.0f;

  aud_ui_text(x, top + 4.0f, 16, AUD_UI_MUTED, state_label(st->state));
  x += 92.0f;

  meter.x = x;
  meter.y = top + 4.0f;
  meter.width = meter_w;
  meter.height = 14.0f;
  if (meter.x + meter.width < r.x + r.width - 260.0f)
  {
    aud_ui_meter(meter, (float)st->peak, a->peak_hold);
    x += meter_w + 10.0f;

    snprintf(text, sizeof(text), "%.1f dBFS", aud_format_dbfs(st->peak));
    aud_ui_text(x, top + 4.0f, 15, st->clipped ? AUD_UI_RECORD : AUD_UI_MUTED, text);
  }

  /*
   * The selection, spelled out. A highlighted band says roughly how much; the
   * numbers are what you need to line one take up against another, and Audacity
   * puts them here for the same reason.
   */
  {
    char from[24];
    char to[24];

    format_position(from, sizeof(from), a->doc.sel_start, a->doc.rate);
    format_position(to, sizeof(to), a->doc.sel_end, a->doc.rate);

    if (aud_doc_has_range(&a->doc))
    {
      snprintf(text, sizeof(text), "selection  %s - %s   (%.3f s)", from, to,
               (double)(a->doc.sel_end - a->doc.sel_start) / a->doc.rate);
    }
    else
    {
      snprintf(text, sizeof(text), "cursor  %s", from);
    }
    aud_ui_text_right(r.x + r.width, top + 4.0f, 15, AUD_UI_MUTED, text);
  }

  /* the second line: what just happened, and what the project is costing */
  {
    const char *say = a->status;
    Color colour = AUD_UI_MUTED;

    if (a->render != NULL)
    {
      snprintf(text, sizeof(text), "rendering %.0f%%",
               aud_render_progress(a->render) * 100.0);
      say = text;
      colour = AUD_UI_ACCENT;
    }
    else if (st->error[0] != '\0')
    {
      say = st->error;
      colour = AUD_UI_RECORD;
    }
    else if (a->render_note[0] != '\0')
    {
      say = a->render_note;
      colour = strncmp(a->render_note, "wrote", 5) == 0 ? AUD_UI_OK : AUD_UI_WARN;
    }
    else if (a->timeline.hint[0] != '\0' && a->status[0] == '\0')
    {
      say = a->timeline.hint;
    }

    aud_ui_text(r.x, r.y + 24.0f, 14, colour, say);
  }

  /*
   * The monitoring level in the units it is thought about in. Down here rather
   * than beside its own slider: the toolbar has no room for it at the minimum
   * window width, and a number about how loud something is belongs next to the
   * meter anyway.
   */
  {
    char level[32];

    format_monitor_gain(level, sizeof(level), a->monitor_gain);
    snprintf(text, sizeof(text), "monitor %s", level);
    aud_ui_text(x + 96.0f, top + 4.0f, 15, AUD_UI_EDGE, text);
  }

  {
    size_t bytes = aud_doc_bytes(&a->doc);

    if (bytes > 0)
    {
      snprintf(text, sizeof(text), "%zu track(s)   %.1f MiB", a->doc.count,
               (double)bytes / (1024.0 * 1024.0));
      aud_ui_text_right(r.x + r.width, r.y + 24.0f, 14, AUD_UI_EDGE, text);
    }
  }
}

/* -- the shortcut list ------------------------------------------------------ */

/*
 * Every key the window answers to. Kept next to the keys themselves rather
 * than only in the manual: a shortcut nobody can find is a shortcut nobody has.
 */
static const char *const help_keys[][2] = {
    {"space", "record, or pause and resume a take"},
    {"S", "stop the take, or cancel a video render"},
    {"L", "play the selection round and round"},
    {"C", "the metronome; it is heard, never recorded"},
    {"G", "count the ruler in bars; alt drops off the grid"},
    {"- / +", "the tempo, a beat at a time; shift for ten"},
    {"I", "import a WAV as a new track"},
    {"M", "playback monitoring on and off"},
    {"B", "show or hide the visualiser panel"},
    {"V", "the next visualiser style"},
    {"1 - 6", "a visualiser style outright"},
    {"drag", "select audio; ctrl+click adds a track to the selection"},
    {"left / right", "move the cursor; ctrl steps clip to clip, shift selects"},
    {"up / down", "the track above or below; shift adds it to the selection"},
    {"home / end", "the start or the end of the project"},
    {"ctrl+A", "select everything"},
    {"ctrl+X / C / V", "cut, copy, paste"},
    {"[ / ]", "fade the selection in, or out"},
    {"del", "delete the selection and close the gap"},
    {"ctrl+Z", "undo; ctrl+shift+Z redoes"},
    {"ctrl+S / O", "save the session; ctrl+shift+S saves it as, ctrl+O opens"},
    {"ctrl+wheel", "zoom; shift+wheel scrolls, wheel walks the tracks"},
    {"F", "fit the selection, or the whole project"},
#ifdef AUDIAKI_HOTRELOAD
    /* only in a development build, which is the only place it exists */
    {"F5", "load the window's code again, keeping this session"},
#endif
    {"?", "this list; Esc closes it"},
};

#define HELP_ROWS ((int)(sizeof(help_keys) / sizeof(help_keys[0])))

static void draw_help(app *a, Rectangle header)
{
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle panel;
  float y;

  /* the window dimmed rather than hidden: this is a note over the app, not a
   * place you have gone to, and the meter behind it is still worth seeing */
  DrawRectangleRec(screen, Fade(BLACK, 0.7f));

  panel.width = 560.0f;
  if (panel.width > screen.width - 2.0f * APP_PAD)
  {
    panel.width = screen.width - 2.0f * APP_PAD;
  }
  panel.height = 70.0f + (float)HELP_ROWS * 26.0f;
  if (panel.height > screen.height - 2.0f * APP_PAD)
  {
    panel.height = screen.height - 2.0f * APP_PAD;
  }
  panel.x = (screen.width - panel.width) / 2.0f;
  panel.y = (screen.height - panel.height) / 2.0f;

  DrawRectangleRounded(panel, 12.0f / panel.height, 8, AUD_UI_PANEL);
  DrawRectangleRoundedLines(panel, 12.0f / panel.height, 8, AUD_UI_ACCENT);

  aud_ui_text(panel.x + 24.0f, panel.y + 18.0f, 20, AUD_UI_TEXT, "Keyboard");

  y = panel.y + 52.0f;
  for (int i = 0; i < HELP_ROWS; i++)
  {
    aud_ui_text(panel.x + 24.0f, y, 15, AUD_UI_ACCENT, help_keys[i][0]);
    aud_ui_text(panel.x + 150.0f, y, 15, AUD_UI_MUTED, help_keys[i][1]);
    y += 26.0f;
  }

  /*
   * A click anywhere dismisses it - having to aim at a close button to get back
   * to the window is worse than the list being in the way. The one exception is
   * the button that opened it, whose own click is already the same release and
   * has flipped the flag itself.
   */
  if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
      !CheckCollisionPointRec(GetMousePosition(), header_help(header)))
  {
    a->help_open = 0;
  }
}

/* -- the two whole screens -------------------------------------------------- */

/*
 * The window when there is no device to draw from. It keeps the picker, so a
 * machine with a second interface is one click away from working rather than a
 * restart away - and it keeps the tracks, because audio already recorded is
 * still worth editing with the interface unplugged.
 */
void app_draw_fatal(app *a)
{
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle header = {APP_PAD, APP_PAD, screen.width - 2.0f * APP_PAD, APP_HEADER_H};
  Rectangle line = screen;
  int previous = a->device_selected;

  ClearBackground(AUD_UI_BG);

  aud_ui_text(header.x, header.y + 6.0f, 26, AUD_UI_TEXT, AUDIAKI_NAME);

  line.height = 34.0f;
  line.y = screen.height / 2.0f - 60.0f;
  aud_ui_text_centred(line, 24, AUD_UI_RECORD, a->fatal);

  line.y += 40.0f;
  aud_ui_text_centred(line, 16, AUD_UI_MUTED,
                      "plug an interface in and it opens by itself - the window is "
                      "watching for one");

  line.y += 26.0f;
  aud_ui_text_centred(line, 16, AUD_UI_MUTED,
                      "the device may also be held by another program");

  /* last, so an open menu covers the message rather than the other way round */
  if (aud_ui_dropdown(header_picker(header), a->device_labels, a->devices.count,
                      &a->device_selected, &a->device_menu_open, &a->device_menu_scroll,
                      !a->save.open))
  {
    app_switch_device(a, previous);
  }

  /*
   * Drawn here too. Losing the capture device does not make the take that was
   * already recorded any less worth filing, and a dialog that vanished with the
   * interface would leave it wherever it happened to land.
   */
  app_save_draw(a);

  aud_ui_tooltip_draw();
}

void app_draw_frame(app *a, const aud_engine_status *st)
{
  float w = (float)GetScreenWidth();
  float h = (float)GetScreenHeight();
  Rectangle header = {APP_PAD, APP_PAD, w - 2.0f * APP_PAD, APP_HEADER_H};
  Rectangle transport;
  Rectangle edits;
  Rectangle viz;
  Rectangle ruler;
  Rectangle tracks;
  Rectangle status;
  int live = st->state == AUD_ENGINE_RECORDING || st->state == AUD_ENGINE_PAUSED;
  int previous = a->device_selected;
  float y;

  ClearBackground(AUD_UI_BG);

  status.x = APP_PAD;
  status.width = w - 2.0f * APP_PAD;
  status.height = APP_STATUS_H + 16.0f;
  status.y = h - status.height;

  y = header.y + header.height + 4.0f;

  transport.x = APP_PAD;
  transport.width = w - 2.0f * APP_PAD;
  transport.height = APP_TOOLBAR_H;
  transport.y = y;
  y += APP_TOOLBAR_H + 4.0f;

  edits = transport;
  edits.y = y;
  y += APP_TOOLBAR_H + 6.0f;

  viz.x = 0.0f;
  viz.width = w;
  viz.y = y;
  viz.height = a->viz_open ? a->viz_height : APP_VIZ_BAR_H;
  /* the tracks come first when the window is short: the visualiser is a
   * readout, and a readout should not push the work off the screen */
  if (viz.y + viz.height > status.y - APP_RULER_H - 80.0f)
  {
    viz.height = status.y - APP_RULER_H - 80.0f - viz.y;
    if (viz.height < APP_VIZ_BAR_H)
    {
      viz.height = APP_VIZ_BAR_H;
    }
  }
  y += viz.height + 4.0f;

  ruler.x = 0.0f;
  ruler.width = w;
  ruler.y = y;
  ruler.height = APP_RULER_H;

  tracks.x = 0.0f;
  tracks.width = w;
  tracks.y = ruler.y + ruler.height;
  tracks.height = status.y - tracks.y - 2.0f;
  if (tracks.height < 40.0f)
  {
    tracks.height = 40.0f;
  }

  draw_header(a, header);
  draw_transport(a, transport, st);
  draw_edit_bar(a, edits,
                (Rectangle){tracks.x + AUD_TIMELINE_PANEL_W + AUD_TIMELINE_SCALE_W,
                            tracks.y,
                            tracks.width - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W,
                            tracks.height});
  draw_viz_panel(a, viz);

  /*
   * The cursor is where the next edit goes; the playhead is where the audio
   * has got to. They are only the same thing when nothing is running, and the
   * arrow keys move one without moving the other - so the timeline is told
   * which is which rather than being handed the cursor twice.
   */
  {
    uint64_t head = a->doc.cursor;
    int running = 0;

    if (a->record_track >= 0)
    {
      head = a->record_at + (uint64_t)(st->elapsed * a->doc.rate);
      running = 1;
    }
    else if (aud_player_playing(&a->player))
    {
      head = aud_player_head(&a->player);
      running = 1;
    }

    aud_timeline_draw(&a->timeline, &a->doc, ruler, tracks, head, running, !covered(a));
  }

  draw_status(a, status, st);

  /*
   * Last, because an open menu has to cover everything under it. It is disabled
   * while a take is open: swapping the device means closing the capture stream,
   * and doing that mid-take would truncate the recording.
   */
  if (aud_ui_dropdown(header_picker(header), a->device_labels, a->devices.count,
                      &a->device_selected, &a->device_menu_open, &a->device_menu_scroll,
                      !live && !a->help_open && !a->save.open))
  {
    app_switch_device(a, previous);
  }
  tip(a, header_picker(header),
      live ? "stop the take before switching device" : "capture device");

  if (live && a->device_menu_open)
  {
    a->device_menu_open = 0;
  }

  if (a->help_open)
  {
    draw_help(a, header);
  }

  /* over even the shortcut list: it is the one thing here waiting on an answer */
  app_save_draw(a);

  /* last of all, so it is over every control that could have asked for it */
  aud_ui_tooltip_draw();
}
