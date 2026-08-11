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

/* Between two buttons, and between two groups of them. */
#define SCREEN_BUTTON_GAP 5.0f
#define SCREEN_GROUP_GAP 18.0f

/*
 * What the toolbars are lettered at when the window has room for it, and the
 * smallest they are packed to before ui.c starts cutting labels short. Both
 * bars use one size, worked out from whichever of them is tighter: two rows of
 * buttons stacked on top of each other in different lettering read as a
 * mistake rather than as a bar that made room for itself.
 */
#define SCREEN_FONT_MAX 18
#define SCREEN_FONT_MIN 12

/* the drawer's strip, which is shorter than a toolbar row */
#define SCREEN_TAB_FONT 15

/* the capture gain slider on the status bar, beside the meter it moves */
#define SCREEN_GAIN_W 96.0f

/*
 * The transport, in the order it is laid out and described by the longest label
 * each slot ever carries. Widths come from these rather than from what the
 * button says this frame: Play becomes Playing while it runs and Save grows a
 * star when there is something to save, and a bar that resized itself around
 * that would shuffle every button along it out from under the pointer.
 */
enum
{
  SCREEN_PLAY = 0,
  SCREEN_LOOP,
  SCREEN_REC,
  SCREEN_PAUSE,
  SCREEN_STOP,
  SCREEN_IMPORT,
  SCREEN_EXPORT,
  SCREEN_OPEN,
  SCREEN_SAVE,
  SCREEN_TRANSPORT_COUNT
};

static const char *const screen_transport[SCREEN_TRANSPORT_COUNT] = {
    "Playing", "Loop",   "Recording", "Resume", "Cancel",
    "Import",  "Export", "Open",      "Save *"};

/* The capture options at the other end of the same row, likewise. */
enum
{
  SCREEN_OVERDUB = 0,
  SCREEN_VIDEO,
  SCREEN_VIDEO_AUDIO,
  SCREEN_MONITOR,
  SCREEN_CAPTURE_COUNT
};

static const char *const screen_capture[SCREEN_CAPTURE_COUNT] = {
    "Overdub", "Video", "No audio", "Monitor on"};

/* Every button on the edit bar, in order, with what each is for. */
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
  return a->device_menu_open || a->help_open || a->save.open || a->confirm.open;
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

/* -- laying a row of buttons out ------------------------------------------- */

/*
 * A toolbar is measured from its labels rather than cut into equal slots. "Cut"
 * does not need the room "Fade out" does, and a bar that hands them the same
 * width either wastes half of one or writes the other over its neighbour - which
 * is what the window did at anything less than full screen.
 */
static float label_pad(int font)
{
  return 0.45f * (float)font;
}

static float button_width(const char *label, int font)
{
  return (float)MeasureText(label, font) + 2.0f * label_pad(font);
}

/* The room `count` buttons need side by side, the gaps between them included. */
static float row_width(const char *const *labels, int count, int font)
{
  float w = 0.0f;

  for (int i = 0; i < count; i++)
  {
    w += button_width(labels[i], font);
  }
  if (count > 1)
  {
    w += SCREEN_BUTTON_GAP * (float)(count - 1);
  }
  return w;
}

/* A button carrying one character still has to be worth aiming at. */
static float step_width(int font)
{
  float w = button_width("+", font);

  return w < 24.0f ? 24.0f : w;
}

static float monitor_slider_width(int font)
{
  return 6.0f * (float)font;
}

/* Lay `labels` out along `row` from `x`, and return the edge they reach. */
static float place_row(Rectangle row, float x, const char *const *labels, int count,
                       int font, Rectangle *out)
{
  for (int i = 0; i < count; i++)
  {
    out[i].x = x;
    out[i].y = row.y;
    out[i].width = button_width(labels[i], font);
    out[i].height = row.height;
    x += out[i].width + SCREEN_BUTTON_GAP;
  }
  return x - SCREEN_BUTTON_GAP;
}

/*
 * The tempo and zoom clusters, which end the edit bar whatever else fits: the
 * metronome, the number it counts on, the grid it draws, and the three that
 * decide how much of the project is on the screen. Four one-character steps,
 * five gaps - the tempo's own three butt up against each other, being one
 * control rather than three.
 */
static float edit_tail_width(int font)
{
  return button_width("Click", font) + button_width("188 BPM", font) +
         button_width("Grid", font) + button_width("[ ]", font) +
         4.0f * step_width(font) + 5.0f * SCREEN_BUTTON_GAP;
}

static float transport_row_width(int font)
{
  return row_width(screen_transport, SCREEN_TRANSPORT_COUNT, font) + SCREEN_GROUP_GAP +
         row_width(screen_capture, SCREEN_CAPTURE_COUNT, font) + SCREEN_BUTTON_GAP +
         monitor_slider_width(font);
}

static float edit_row_width(int font)
{
  float w = 0.0f;

  for (int i = 0; i < SCREEN_EDIT_COUNT; i++)
  {
    w += button_width(screen_edits[i].label, font) + SCREEN_BUTTON_GAP;
  }
  return w - SCREEN_BUTTON_GAP + SCREEN_GROUP_GAP + edit_tail_width(font);
}

/*
 * The size both toolbars are lettered at: the largest at which each of them
 * fits the window whole. Nothing is dropped or overlapped above the floor, and
 * below it ui.c cuts the longest labels short - which is still a bar every
 * button of which can be read and pressed.
 */
static int toolbar_font(float row_w)
{
  int font;

  for (font = SCREEN_FONT_MAX; font > SCREEN_FONT_MIN; font--)
  {
    if (transport_row_width(font) <= row_w && edit_row_width(font) <= row_w)
    {
      break;
    }
  }
  return font;
}

/* -- the transport bar ------------------------------------------------------ */

static void draw_transport(app *a, Rectangle r, const aud_engine_status *st, int font)
{
  int recording = st->state == AUD_ENGINE_RECORDING;
  int paused = st->state == AUD_ENGINE_PAUSED;
  int rendering = a->render != NULL;
  int live = (recording || paused) && !covered(a);
  int usable = a->engine != NULL && st->state != AUD_ENGINE_FAILED && !covered(a);
  int playing = aud_player_playing(&a->player);
  Rectangle slot[SCREEN_TRANSPORT_COUNT];
  Rectangle play;
  Rectangle loop;
  Rectangle rec;
  Rectangle pause;
  Rectangle stop;
  Rectangle import;
  Rectangle export_to;
  Rectangle open_project;
  Rectangle save_project;
  float filled = place_row(r, r.x, screen_transport, SCREEN_TRANSPORT_COUNT, font, slot);

  play = slot[SCREEN_PLAY];
  loop = slot[SCREEN_LOOP];
  rec = slot[SCREEN_REC];
  pause = slot[SCREEN_PAUSE];
  stop = slot[SCREEN_STOP];
  import = slot[SCREEN_IMPORT];
  export_to = slot[SCREEN_EXPORT];
  open_project = slot[SCREEN_OPEN];
  save_project = slot[SCREEN_SAVE];

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
    float slider_w = monitor_slider_width(font);
    float group_w = row_width(screen_capture, SCREEN_CAPTURE_COUNT, font) +
                    SCREEN_BUTTON_GAP + slider_w;
    Rectangle group[SCREEN_CAPTURE_COUNT];
    Rectangle slider = {r.x + r.width - slider_w, r.y + (r.height - 22.0f) / 2.0f,
                        slider_w, 22.0f};
    Rectangle overdub;
    Rectangle video;
    Rectangle audio;
    Rectangle monitor;
    int wanted = a->engine != NULL && aud_engine_monitor_wanted(a->engine);
    /*
     * Only settable between takes: the video is rendered from the finished
     * WAV, so changing your mind halfway through would be answered either by
     * rendering the whole take or none of it, and neither is what the click
     * meant.
     */
    int settable = usable && !live && !rendering;

    if (r.x + r.width - group_w < filled + SCREEN_GROUP_GAP)
    {
      return; /* too narrow a window for these; the transport comes first */
    }

    place_row(r, r.x + r.width - group_w, screen_capture, SCREEN_CAPTURE_COUNT, font,
              group);
    overdub = group[SCREEN_OVERDUB];
    video = group[SCREEN_VIDEO];
    audio = group[SCREEN_VIDEO_AUDIO];
    monitor = group[SCREEN_MONITOR];

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

/*
 * The metronome, the tempo and the grid, drawn as one control at the end of
 * the edit bar. The reading between the two steps is text rather than a field:
 * a tempo is arrived at by nudging it against what you are playing until it
 * sits right, not by typing a number you already knew.
 */
static void draw_tempo_cluster(app *a, Rectangle click, Rectangle slower,
                               Rectangle reading, Rectangle faster, Rectangle grid,
                               int font)
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
  aud_ui_text_centred(reading, font, a->click_on ? AUD_UI_TEXT : AUD_UI_MUTED, text);
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
  if (a->timeline.grid)
  {
    char what[64];

    snprintf(what, sizeof(what),
             "snapping to %s; shift+G divides it, alt drops off it   G",
             aud_doc_grid_label(&a->doc));
    tip(a, grid, what);
  }
  else
  {
    tip(a, grid, "count the ruler in bars, and put edits on the grid   G");
  }
}

static void draw_edit_bar(app *a, Rectangle r, Rectangle wave_area, int font)
{
  int usable = !covered(a);
  float step = step_width(font);
  float zoom_w = 2.0f * step + button_width("[ ]", font) + 2.0f * SCREEN_BUTTON_GAP;
  Rectangle zoom_out = {r.x + r.width - zoom_w, r.y, step, r.height};
  Rectangle zoom_in = {zoom_out.x + step + SCREEN_BUTTON_GAP, r.y, step, r.height};
  Rectangle fit = {zoom_in.x + step + SCREEN_BUTTON_GAP, r.y, button_width("[ ]", font),
                   r.height};
  Rectangle grid = {zoom_out.x - SCREEN_BUTTON_GAP - button_width("Grid", font), r.y,
                    button_width("Grid", font), r.height};
  /* the three of these butt up against each other: one control, not three */
  Rectangle faster = {grid.x - SCREEN_BUTTON_GAP - step, r.y, step, r.height};
  Rectangle reading = {faster.x - button_width("188 BPM", font), r.y,
                       button_width("188 BPM", font), r.height};
  Rectangle slower = {reading.x - step, r.y, step, r.height};
  Rectangle click = {slower.x - SCREEN_BUTTON_GAP - button_width("Click", font), r.y,
                     button_width("Click", font), r.height};
  /*
   * What is left for the edits, once the clusters at the far end have had
   * theirs. Each of them then takes the width its own label asks for: they are
   * three characters long at one end of the row and eight at the other, and a
   * bar that gave them all the same either wastes the short ones' room or spills
   * the long ones over the button next door.
   */
  Rectangle row = r;
  float x;

  row.width = click.x - SCREEN_GROUP_GAP - row.x;

  x = row.x;
  for (int i = 0; i < SCREEN_EDIT_COUNT; i++)
  {
    Rectangle slot = {x, row.y, button_width(screen_edits[i].label, font), row.height};
    int enabled = usable;

    if (slot.x + slot.width > row.x + row.width)
    {
      break; /* the window is too narrow for the rest; zoom stays reachable */
    }
    x += slot.width + SCREEN_BUTTON_GAP;

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

  draw_tempo_cluster(a, click, slower, reading, faster, grid, font);

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

/* -- the drawer ------------------------------------------------------------- */

/*
 * The drawer: the visualiser, which used to be the window and is now a panel in
 * it, and the spectrum editor beside it.
 *
 * One at a time, sharing the same room. They are both a wide short picture
 * under the toolbars and there is only one such place on the screen, so a
 * window offering both at once would be a window with no tracks left in it -
 * and they answer different questions anyway. The visualiser is what is coming
 * in now, which is a thing to watch while playing; the spectrum is what was
 * recorded, which is a thing to work on afterwards.
 */
static void draw_drawer(app *a, Rectangle r)
{
  Rectangle bar = r;
  Rectangle stage;
  Rectangle viz_tab;
  Rectangle fix_tab;

  bar.height = APP_VIZ_BAR_H;

  DrawRectangleRec(bar, AUD_UI_PANEL);

  /*
   * The strip is short, so its two names are lettered to fit it rather than to
   * match the toolbars, and each is as wide as what it says.
   */
  aud_ui_label_size(SCREEN_TAB_FONT);

  viz_tab.x = bar.x;
  viz_tab.y = bar.y;
  viz_tab.width = button_width("v  Visualiser", SCREEN_TAB_FONT);
  viz_tab.height = bar.height;

  fix_tab = viz_tab;
  fix_tab.x = viz_tab.x + viz_tab.width + 2.0f;
  fix_tab.width = button_width("v  Spectrum", SCREEN_TAB_FONT);

  if (aud_ui_toggle(viz_tab,
                    a->drawer == APP_DRAWER_VIZ ? "v  Visualiser" : ">  Visualiser",
                    a->drawer == APP_DRAWER_VIZ, AUD_UI_ACCENT, !covered(a)))
  {
    a->drawer = a->drawer == APP_DRAWER_VIZ ? APP_DRAWER_NONE : APP_DRAWER_VIZ;
  }
  tip(a, viz_tab, "show or hide the live visualiser   B");

  if (aud_ui_toggle(fix_tab,
                    a->drawer == APP_DRAWER_SPECTRUM ? "v  Spectrum" : ">  Spectrum",
                    a->drawer == APP_DRAWER_SPECTRUM, AUD_UI_ACCENT, !covered(a)))
  {
    a->drawer = a->drawer == APP_DRAWER_SPECTRUM ? APP_DRAWER_NONE : APP_DRAWER_SPECTRUM;
    aud_repair_panel_reset(&a->repair);
  }
  tip(a, fix_tab, "the spectrum of the selected audio, and taking noise out of it   N");

  aud_ui_label_size(0);

  stage = (Rectangle){r.x, bar.y + bar.height, r.width, r.height - bar.height};

  if (a->drawer == APP_DRAWER_SPECTRUM)
  {
    aud_repair_panel_draw(&a->repair, &a->doc, stage, !covered(a));

    if (a->repair.apply_wanted)
    {
      a->repair.apply_wanted = 0;
      app_confirm_apply(a, aud_repair_panel_seconds(&a->repair, &a->doc),
                        aud_repair_panel_track(&a->repair, &a->doc));
    }
    return;
  }

  if (a->drawer != APP_DRAWER_VIZ || a->viz == NULL)
  {
    return;
  }

  {
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

/*
 * How a capture gain is said out loud: in dB, because that is the unit an
 * input trim is thought in, with the multiplier kept out of sight. "off"
 * rather than "-inf dB" at the bottom of the travel.
 */
static void format_input_gain(char *dst, size_t size, float gain)
{
  if (!(gain > 0.0f))
  {
    snprintf(dst, size, "off");
    return;
  }
  snprintf(dst, size, "%+.1f dB", 20.0 * log10((double)gain));
}

static void draw_status(app *a, Rectangle r, const aud_engine_status *st)
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
    x += 76.0f;

    /*
     * The capture gain, next to the meter it moves - which is the whole point
     * of it being here rather than up in the transport bar with the monitoring
     * level. Setting an input level is something you do while watching a
     * needle, and the needle is here.
     *
     * Only drawn when there is room for it and the readout beside it; on a
     * narrow window the meter is worth more than the knob, and the knob is
     * still reachable from the command line.
     */
    if (x + SCREEN_GAIN_W + 84.0f < r.x + r.width - 260.0f)
    {
      Rectangle gain = {x, top + 3.0f, SCREEN_GAIN_W, 16.0f};
      int usable = a->engine != NULL && st->state != AUD_ENGINE_FAILED && !covered(a);
      char level[32];

      if (aud_ui_slider(gain, &a->input_gain, (float)AUD_GAIN_MIN, (float)AUD_GAIN_MAX,
                        AUD_UI_WARN, usable))
      {
        aud_engine_set_input_gain(a->engine, a->input_gain);
      }
      tip(a, gain,
          "gain added to the recording itself, silent to +24 dB - watch the meter, "
          "this one can clip the take");

      x += SCREEN_GAIN_W + 8.0f;
      format_input_gain(level, sizeof(level), a->input_gain);
      snprintf(text, sizeof(text), "in %s", level);
      aud_ui_text(x, top + 4.0f, 15, a->input_gain > 1.0f ? AUD_UI_WARN : AUD_UI_MUTED,
                  text);
      x += 84.0f;
    }
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
   *
   * After the capture gain above and dimmer than it, which is the order they
   * matter in: one of these changes the recording and the other only changes
   * what you hear while making it.
   */
  {
    char level[32];

    format_monitor_gain(level, sizeof(level), a->monitor_gain);
    snprintf(text, sizeof(text), "monitor %s", level);
    if (x + 130.0f < r.x + r.width - 260.0f)
    {
      aud_ui_text(x + 16.0f, top + 4.0f, 15, AUD_UI_EDGE, text);
    }
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
    {"N", "the spectrum of the selected audio, and taking noise out of it"},
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
int app_draw_fatal(app *a)
{
  int quit = 0;
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

  /* asked here too: losing the interface is not a reason to lose the edits */
  quit = app_confirm_draw(a);

  aud_ui_tooltip_draw();
  return quit;
}

int app_draw_frame(app *a, const aud_engine_status *st)
{
  int quit = 0;
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
  viz.height = APP_VIZ_BAR_H;
  if (a->drawer == APP_DRAWER_VIZ)
  {
    viz.height = a->viz_height;
  }
  else if (a->drawer == APP_DRAWER_SPECTRUM)
  {
    viz.height = APP_FIX_OPEN_H;
  }
  /* the tracks come first when the window is short: the drawer is a readout,
   * and a readout should not push the work off the screen */
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

  /*
   * Both bars packed to one size, worked out from the window as it is now: it
   * is resizable, and a toolbar laid out for the width it was opened at is a
   * toolbar with buttons under each other at any other width.
   */
  {
    int font = toolbar_font(transport.width);

    aud_ui_label_size(font);
    draw_transport(a, transport, st, font);
    draw_edit_bar(a, edits,
                  (Rectangle){tracks.x + AUD_TIMELINE_PANEL_W + AUD_TIMELINE_SCALE_W,
                              tracks.y,
                              tracks.width - AUD_TIMELINE_PANEL_W - AUD_TIMELINE_SCALE_W,
                              tracks.height},
                  font);
    aud_ui_label_size(0);
  }

  draw_drawer(a, viz);

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

  /* the close button on a lane asks rather than acts; see timeline.h */
  if (a->timeline.close_requested >= 0)
  {
    long index = a->timeline.close_requested;

    a->timeline.close_requested = -1;
    app_confirm_close_track(a, (size_t)index);
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

  /* over even that: a question stops everything, the save dialog included */
  quit = app_confirm_draw(a);

  /* last of all, so it is over every control that could have asked for it */
  aud_ui_tooltip_draw();
  return quit;
}
