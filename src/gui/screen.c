/* SPDX-License-Identifier: MIT */
/*
 * screen.c - every pixel audiaki-gui draws.
 *
 * Apart from main.c because it is the half that changes when the window is
 * redesigned rather than when the recorder is: it reads `app` and the engine's
 * status, and the only writing it does is through the transport actions the
 * buttons stand for. See app.h.
 */
#include "gui/app.h"

#include "gui/ui.h"
#include "gui/viz.h"

#include "audio/format.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The device dropdown's slot, which the header leaves clear for it. */
static Rectangle header_picker(Rectangle r)
{
  Rectangle picker;

  picker.width = 260.0f;
  if (picker.width > r.width * 0.4f)
  {
    picker.width = r.width * 0.4f;
  }
  picker.height = 32.0f;
  picker.x = r.x + r.width - picker.width;
  picker.y = r.y + (r.height - picker.height) / 2.0f;
  return picker;
}

/* The shortcut list's button, tucked in beside the picker. */
static Rectangle header_help(Rectangle r)
{
  Rectangle picker = header_picker(r);
  Rectangle help;

  help.width = 30.0f;
  help.height = 30.0f;
  help.x = picker.x - 10.0f - help.width;
  help.y = picker.y + (picker.height - help.height) / 2.0f;
  return help;
}

/*
 * Hover help, suppressed while the menu or the shortcut list is over the top:
 * a tooltip for a control the user cannot currently reach is noise.
 */
static void tip(const app *a, Rectangle bounds, const char *text)
{
  if (a->device_menu_open || a->help_open || a->save.open)
  {
    return;
  }

  aud_ui_tooltip(bounds, text);
}

static void draw_header(app *a, Rectangle r)
{
  Rectangle help = header_help(r);
  char detail[128];

  aud_ui_text(r.x, r.y + 6.0f, 28, AUD_UI_TEXT, AUDIAKI_NAME);

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
  snprintf(detail, sizeof(detail), "%u Hz   %u ch   %s", aud_engine_rate(a->engine),
           aud_engine_channels(a->engine), aud_format_name(aud_engine_format(a->engine)));
  aud_ui_text_right(help.x - 16.0f, r.y + 14.0f, 18, AUD_UI_MUTED, detail);
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

static void draw_transport(app *a, Rectangle r, const aud_engine_status *st)
{
  float bw = 120.0f;
  float gap = 10.0f;
  Rectangle rec = {r.x, r.y, bw, r.height};
  Rectangle pause = {r.x + bw + gap, r.y, bw, r.height};
  Rectangle stop = {r.x + 2.0f * (bw + gap), r.y, bw, r.height};
  int recording = st->state == AUD_ENGINE_RECORDING;
  int paused = st->state == AUD_ENGINE_PAUSED;
  int rendering = a->render != NULL;
  /* an open menu, dialog or shortcut list covers these, so none takes a click */
  int covered = a->device_menu_open || a->help_open || a->save.open;
  int live = (recording || paused) && !covered;
  int usable = st->state != AUD_ENGINE_FAILED && !covered;

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
  else if (st->state == AUD_ENGINE_FAILED)
  {
    tip(a, rec, "no capture device");
  }
  else
  {
    tip(a, rec, "start the next numbered take   space");
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

  if (paused)
  {
    tip(a, pause, "carry on writing to the same file   space");
  }
  else if (recording)
  {
    tip(a, pause, "stop writing without closing the file   space");
  }
  else
  {
    tip(a, pause, "nothing to pause - no take is open");
  }

  /*
   * The same slot stops the take and, once the take is stopped and its video
   * is being written, abandons that. Both are "I have had enough of this".
   */
  if (rendering)
  {
    if (aud_ui_button(stop, "Cancel", AUD_UI_WARN, !covered))
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
        (recording || paused) ? "close the take and patch its header   S"
                              : "nothing to stop - no take is open");
  }

  /* the capture options sit at the right hand end, away from the transport */
  {
    float slider_w = 140.0f;
    float monitor_w = 120.0f;
    float video_w = 100.0f;
    float audio_w = 100.0f;
    Rectangle slider = {r.x + r.width - slider_w, r.y + (r.height - 26.0f) / 2.0f,
                        slider_w, 26.0f};
    Rectangle monitor = {slider.x - gap - monitor_w, r.y, monitor_w, r.height};
    Rectangle audio = {monitor.x - gap - audio_w, r.y, audio_w, r.height};
    Rectangle video = {audio.x - gap - video_w, r.y, video_w, r.height};
    int wanted = aud_engine_monitor_wanted(a->engine);
    /*
     * Only settable between takes: the video is rendered from the finished
     * WAV, so changing your mind halfway through would be answered either by
     * rendering the whole take or none of it, and neither is what the click
     * meant.
     */
    int settable = usable && !live && !rendering;

    if (aud_ui_toggle(video, "Video", a->want_video, AUD_UI_ACCENT, settable))
    {
      a->want_video = !a->want_video;
    }
    tip(a, video,
        settable ? "also render an MP4 of the visualiser when the take stops"
                 : "only settable between takes");

    /*
     * What goes in that video, so it sits next to the control that turns it on
     * and greys out with it. The label says which way it is set rather than
     * leaving that to the lit state alone: "no audio" is the answer people
     * come looking for, and it should be readable without clicking anything.
     */
    if (aud_ui_toggle(audio, a->want_video_audio ? "Audio" : "No audio",
                      a->want_video_audio, AUD_UI_ACCENT, settable && a->want_video))
    {
      a->want_video_audio = !a->want_video_audio;
    }

    if (!a->want_video)
    {
      tip(a, audio, "turn Video on first");
    }
    else
    {
      tip(a, audio,
          settable ? "whether that video carries the take's own audio"
                   : "only settable between takes");
    }

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

    /*
     * The level as a number, read after the slider so a drag shows where it is
     * now rather than where it was a frame ago. The knob alone says "somewhere
     * near the middle", which is not a level you can set deliberately or come
     * back to tomorrow.
     */
    {
      char level[32];

      format_monitor_gain(level, sizeof(level), a->monitor_gain);
      aud_ui_text_right(slider.x + slider.width, r.y + 1.0f, 14,
                        usable ? AUD_UI_MUTED : AUD_UI_EDGE, level);
    }
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
   * One line on the right for whatever most needs saying: the video being
   * rendered while that is happening, then a failure, then clipping, then the
   * file being written.
   */
  if (a->render != NULL)
  {
    const char *path = aud_render_output(a->render);
    const char *slash = strrchr(path, '/');
    float pct = (float)aud_render_progress(a->render) * 100.0f;
    Rectangle bar;

    snprintf(right, sizeof(right), "rendering %.120s   %.0f%%",
             slash != NULL ? slash + 1 : path, pct);
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_ACCENT, right);

    bar.width = 160.0f;
    bar.height = 4.0f;
    bar.x = r.x + r.width - bar.width;
    bar.y = r.y + 30.0f;
    DrawRectangleRec(bar, AUD_UI_EDGE);
    DrawRectangleRec((Rectangle){bar.x, bar.y, bar.width * (pct / 100.0f), bar.height},
                     AUD_UI_ACCENT);
    return;
  }

  if (st->error[0] != '\0')
  {
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_RECORD, st->error);
    return;
  }

  /* how the last render went, until the next take replaces it */
  if (a->render_note[0] != '\0')
  {
    int good = strncmp(a->render_note, "wrote", 5) == 0;

    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, good ? AUD_UI_OK : AUD_UI_WARN,
                      a->render_note);
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
    {
      snprintf(right, sizeof(right), "%.200s   %.1f MiB   %u xrun(s)", name,
               (double)st->bytes / (1024.0 * 1024.0), st->xruns);
    }
    else
    {
      snprintf(right, sizeof(right), "%.200s   %.1f MiB", name,
               (double)st->bytes / (1024.0 * 1024.0));
    }

    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_MUTED, right);
  }
  /* what pressing record would give you, which with a pre-roll starts earlier */
  else if (st->preroll_size > 0.0)
  {
    snprintf(right, sizeof(right), "next take: %.180s-...   pre-roll %.1f s", a->prefix,
             st->preroll_held);
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_MUTED, right);
  }
  else
  {
    snprintf(right, sizeof(right), "next take: %.200s-...", a->prefix);
    aud_ui_text_right(r.x + r.width, r.y + 10.0f, 18, AUD_UI_MUTED, right);
  }
}

/*
 * Every key the window answers to. Kept next to the keys themselves rather
 * than only in the manual: a shortcut nobody can find is a shortcut nobody has.
 */
static const char *const help_keys[][2] = {
    {"space", "record, or pause and resume a take"},
    {"S", "stop the take, or cancel a video render"},
    {"M", "playback monitoring on and off"},
    {"V", "the next visualiser style"},
    {"1 - 6", "a visualiser style outright"},
    {"F", "fullscreen"},
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

  panel.width = 520.0f;
  if (panel.width > screen.width - 2.0f * APP_PAD)
  {
    panel.width = screen.width - 2.0f * APP_PAD;
  }
  panel.height = 78.0f + (float)HELP_ROWS * 30.0f;
  panel.x = (screen.width - panel.width) / 2.0f;
  panel.y = (screen.height - panel.height) / 2.0f;

  DrawRectangleRounded(panel, 12.0f / panel.height, 8, AUD_UI_PANEL);
  DrawRectangleRoundedLines(panel, 12.0f / panel.height, 8, AUD_UI_ACCENT);

  aud_ui_text(panel.x + 26.0f, panel.y + 22.0f, 22, AUD_UI_TEXT, "Keyboard");

  y = panel.y + 62.0f;
  for (int i = 0; i < HELP_ROWS; i++)
  {
    aud_ui_text(panel.x + 26.0f, y, 18, AUD_UI_ACCENT, help_keys[i][0]);
    aud_ui_text(panel.x + 130.0f, y, 18, AUD_UI_MUTED, help_keys[i][1]);
    y += 30.0f;
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

/*
 * The window when there is no device to draw from. It keeps the picker, so a
 * machine with a second interface is one click away from working rather than a
 * restart away.
 */
void app_draw_fatal(app *a)
{
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle header = {APP_PAD, APP_PAD, screen.width - 2.0f * APP_PAD, APP_HEADER_H};
  Rectangle line = screen;
  int previous = a->device_selected;

  ClearBackground(AUD_UI_BG);

  aud_ui_text(header.x, header.y + 6.0f, 28, AUD_UI_TEXT, AUDIAKI_NAME);

  line.height = 40.0f;
  line.y = screen.height / 2.0f - 70.0f;
  aud_ui_text_centred(line, 28, AUD_UI_RECORD, a->fatal);

  line.y += 46.0f;
  aud_ui_text_centred(line, 18, AUD_UI_MUTED,
                      "plug an interface in and it opens by itself - the window is "
                      "watching for one");

  line.y += 30.0f;
  aud_ui_text_centred(line, 18, AUD_UI_MUTED,
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
      {
        tabs_w = stage.width - 24.0f;
      }

      tabs.x = stage.x + stage.width - 12.0f - tabs_w;
      tabs.y = stage.y + 12.0f;
      tabs.width = tabs_w;
      tabs.height = 26.0f;

      if (aud_ui_tabs(tabs, a->style_labels, AUD_VIZ_MODE_COUNT, &a->style_selected,
                      !a->device_menu_open && !a->help_open && !a->save.open, 1))
      {
        aud_viz_set_mode(a->viz, (aud_viz_mode)a->style_selected);
      }
      tip(a, tabs, "visualiser style   V, or 1 - 6");
    }
  }

  draw_transport(a, transport, st);
  draw_status(a, status, st);

  /*
   * Last, because an open menu has to cover the stage and the transport. It is
   * disabled while a take is open: swapping the device means closing the
   * capture stream, and doing that mid-take would truncate the recording.
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