/* SPDX-License-Identifier: MIT */
/*
 * app.h - the desktop app's own state, shared between its four halves.
 *
 * Internal to src/gui: nothing outside the window includes this. The window is
 * one program split by what the code is doing rather than by what it is doing
 * it to, because `app` is genuinely one object and pretending otherwise would
 * mean threading a dozen pointers through the drawing:
 *
 *   main.c     the run loop, the engine's lifecycle, and the transport actions
 *   args.c     argv and the help text
 *   devices.c  the dropdown's list, and keeping it level with the hardware
 *   screen.c   every pixel
 *
 * screen.c calls the transport actions and main.c calls the drawing, which is
 * the one cycle here and the usual one for a window: what is on screen is a
 * function of the state, and clicking what is on screen changes it.
 */
#ifndef AUDIAKI_GUI_APP_H
#define AUDIAKI_GUI_APP_H

#include "gui/engine.h"
#include "gui/render.h"
#include "gui/viz.h"

#include "backend/backend.h"
#include "backend/device.h"

#include <stdio.h>

#define APP_WIDTH 1100
#define APP_HEIGHT 680
/* wide enough for the transport, the video, audio and monitor controls and the slider */
#define APP_MIN_WIDTH 960
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
 * Seconds of each displayed frame given over to encoding. Generous enough to
 * render several times faster than real time, small enough that the window
 * still redraws while it happens.
 */
#define APP_RENDER_BUDGET 0.012

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

/* What the dropdown offers, rebuilt whenever the hardware underneath changes. */
typedef struct
{
  char name[APP_MAX_DEVICES][64]; /* the string handed to ALSA */
  char label[APP_MAX_DEVICES][APP_DEVICE_LABEL];
  int count;
  int absent; /* the row kept for a device ALSA did not report, or -1 */
} app_devices;

typedef struct
{
  aud_engine *engine;
  aud_viz *viz;
  aud_engine_config cfg;

  char prefix[512];
  /*
   * What monitoring the next engine to open should come up with: -M at
   * startup, and whatever was on when a device was lost and later returned.
   */
  int start_monitor;

  /*
   * The device list, and the watch that keeps it honest: plugging an interface
   * in should put it in the menu without the window being restarted around it.
   */
  app_devices devices;
  aud_device_watch *watch;
  const char *device_labels[APP_MAX_DEVICES]; /* what the dropdown reads */

  /*
   * The device the engine holds, kept here rather than as a pointer into the
   * list: the list is rebuilt underneath it, and the engine keeps the string
   * it was opened with for its whole lifetime.
   */
  char active_device[64];

  int device_selected;
  int device_menu_open;
  int device_menu_scroll; /* top visible row, for a list longer than the menu */

  /* the shortcut list, over the top of everything while it is up */
  int help_open;

  /*
   * What the title bar last said. A window behind another one should still be
   * able to answer "is it still recording?", and setting the title is a round
   * trip to the display server, so it is only set when the answer changes.
   */
  char title[160];

  aud_backend_kind backend; /* chosen once, before the first enumeration */

  /* the visualiser style selector, mirroring the mode held by aud_viz */
  const char *style_labels[AUD_VIZ_MODE_COUNT];
  int style_selected;

  /*
   * Video capture. The take is always written as a WAV; `want_video` decides
   * whether stopping also renders an MP4 of the visualiser from it.
   */
  int want_video;
  /*
   * Whether that MP4 carries the take's own audio. On by default - a take and
   * its visualiser belong together - but a video headed for an edit that has
   * the audio already, or for somewhere it should not play, is better off
   * without a track to strip back out.
   */
  int want_video_audio;
  unsigned video_width;
  unsigned video_height;
  unsigned video_fps;
  aud_render *render;                     /* non-NULL while a video is being written */
  char render_note[AUD_ENGINE_ERROR_MAX]; /* what happened to the last one */

  float peak_hold;
  float peak_hold_left; /* seconds the marker still has before it decays */
  float monitor_gain;

  /* the reason the engine could not be created, if it could not be */
  char fatal[AUD_ENGINE_ERROR_MAX];

  float drain[APP_DRAIN];
} app;

/* -- args.c ---------------------------------------------------------------- */

void app_usage(FILE *out, const app *a);

/* Returns 0 to carry on, -1 when --help was answered, or an exit code to stop with. */
int app_parse_args(app *a, int argc, char **argv);

/* -- devices.c ------------------------------------------------------------- */

void app_load_devices(app *a);

/*
 * Re-walk the backend. Returns 1 when the list changed, which is the only time
 * it is swapped in: the walk happens every couple of seconds, and rebuilding on
 * a timer would shuffle rows under a pointer that is about to click one.
 */
int app_refresh_devices(app *a);

/* -- main.c ---------------------------------------------------------------- */

/* Reopen on the row the dropdown now points at, falling back to `previous`. */
void app_switch_device(app *a, int previous);

void app_begin_take(app *a);
void app_stop_take(app *a, const aud_engine_status *st);
void app_toggle_record(app *a, const aud_engine_status *st);
void app_cancel_render(app *a);

/* -- screen.c -------------------------------------------------------------- */

void app_draw_frame(app *a, const aud_engine_status *st);

/* The window with no engine behind it: a message, and the picker to escape by. */
void app_draw_fatal(app *a);

#endif /* AUDIAKI_GUI_APP_H */
