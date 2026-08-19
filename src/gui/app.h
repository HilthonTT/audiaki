/* SPDX-License-Identifier: MIT */
/*
 * app.h - the desktop app's own state, shared between its parts.
 *
 * Internal to src/gui: nothing outside the window includes this. The window is
 * one program split by what the code is doing rather than by what it is doing
 * it to, because `app` is genuinely one object and pretending otherwise would
 * mean threading a dozen pointers through the drawing:
 *
 *   main.c     the shell: the window, the run loop, and the hot reload key
 *   plug.c     the app's own lifecycle - start, frame, and the way out
 *   take.c     the capture device, and the take being written to it
 *   actions.c  what the toolbar, the keys and the timeline all mean
 *   keys.c     which of those a frame of the keyboard was asking for
 *   args.c     argv and the help text
 *   devices.c  the dropdown's list, and keeping it level with the hardware
 *   save.c     where a finished take goes, and the dialog that asks
 *   preview.c  hearing a file from that dialog without loading it
 *   confirm.c  the question that stops an action until it is answered
 *   screen.c   every pixel of the chrome
 *   timeline.c every pixel of the tracks, and what a pointer does to them
 *
 * screen.c calls the actions and plug.c calls the drawing, which is the one
 * cycle here and the usual one for a window: what is on screen is a function of
 * the state, and clicking what is on screen changes it. save.c is both at once
 * for the one dialog it owns, which is why it is its own file.
 *
 * The three in the middle are one story split where it can be tested. keys.c
 * decides and actions.c does, so what a keystroke means can be checked without
 * a window - see keys.h, which is the only header here that is not this one.
 *
 * main.c is apart from all of it: in a development build everything else here
 * is a library it loads, and can load again while the window is open. That is
 * also why this struct is allocated rather than static, and why nothing in it
 * may point at anything in the code - see hotreload/plug.h.
 */
#ifndef AUDIAKI_GUI_APP_H
#define AUDIAKI_GUI_APP_H

#include "gui/engine.h"
#include "gui/player.h"
#include "gui/preview.h"
#include "gui/render.h"
#include "gui/repair.h"
#include "gui/timeline.h"
#include "gui/viz.h"

#include "backend/backend.h"
#include "backend/device.h"
#include "edit/edit.h"
#include "util/log.h"
#include "util/path.h"

#include <stdio.h>

#define APP_WIDTH 1100
#define APP_HEIGHT 680
/* wide enough for the transport, the video, audio and monitor controls and the slider */
#define APP_MIN_WIDTH 960
#define APP_MIN_HEIGHT 460

#define APP_PAD 14.0f
#define APP_HEADER_H 40.0f
#define APP_TOOLBAR_H 34.0f
#define APP_RULER_H 26.0f
#define APP_STATUS_H 34.0f

/*
 * The drawer under the toolbars: a strip with the two panels' names on it, and
 * whichever of them is open below that.
 *
 * The spectrum wants more room than the visualiser does. One is a readout and
 * reads fine at a glance; the other is a graph with two rows of controls under
 * it and a spike you have to be able to aim at.
 */
#define APP_VIZ_BAR_H 24.0f
#define APP_VIZ_OPEN_H 190.0f
#define APP_FIX_OPEN_H 320.0f
#define APP_VIZ_MIN_H 90.0f

/* Which of the drawer's panels is showing, if either. */
typedef enum
{
  APP_DRAWER_NONE = 0,
  APP_DRAWER_VIZ,     /* the live spectrum coming off the interface */
  APP_DRAWER_SPECTRUM /* the spectrum of what is on the timeline, and editing it */
} app_drawer;

/* What the bottom line says after something happened, until something else does. */
#define APP_STATUS_MAX 200

/*
 * Files that may be named on the command line. More than this is a glob that
 * got away from someone rather than a session, and the project has a ceiling of
 * its own a little above it.
 */
#define APP_MAX_OPEN 32

/*
 * The take drain buffer, as a flat sample count: a quarter of a second at
 * sixteen channels. It is allocated once, before there is a device to ask, so
 * it is sized in samples and divided by the channel count the engine actually
 * negotiated - see app.take_buf_frames, which is what the drain is asked for.
 *
 * Sixteen is a shape, not a limit: -c accepts up to AUD_CHANNELS_MAX, and an
 * interface that wide simply drains fewer frames per pass rather than writing
 * past the end of this.
 */
#define APP_TAKE_BUF_CHANNELS 16u
#define APP_TAKE_BUF_SAMPLES (16384u * APP_TAKE_BUF_CHANNELS)

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

/*
 * Sub-folders the save dialog lists at once. A folder with more than this many
 * directories in it is not one anybody is about to find a take in by looking,
 * and the path can still be typed.
 */
#define APP_MAX_FOLDERS 512

/* A single filename, at the length Linux filesystems stop at. */
#define APP_NAME_MAX 256

/* Which field of the save dialog the keyboard is talking to. */
typedef enum
{
  APP_SAVE_FIELD_NAME = 0,
  APP_SAVE_FIELD_FOLDER,
  APP_SAVE_FIELD_COUNT,
} app_save_field;

/*
 * The dialog that opens when a take stops: where to keep it, and what to call
 * it. Nothing here has happened to the file yet - `take` is a complete, closed
 * WAV sitting exactly where it was recorded, and it stays there until either
 * button is pressed. Closing the window, or the dialog, keeps it.
 */
/* Which question the browser is asking. */
typedef enum
{
  APP_SAVE_MODE_KEEP = 0,     /* where should this finished take go? */
  APP_SAVE_MODE_OPEN,         /* which file should come in as a track? */
  APP_SAVE_MODE_EXPORT,       /* where should the mixed-down project be written? */
  APP_SAVE_MODE_STEMS,        /* ...and where should one WAV a track go? */
  APP_SAVE_MODE_PROJECT_SAVE, /* where should the session itself be written? */
  APP_SAVE_MODE_PROJECT_OPEN, /* which session should be opened? */
} app_save_mode;

/* Whether `mode` writes audio out of the project, as one file or as many. */
#define APP_SAVE_IS_EXPORT(mode) \
  ((mode) == APP_SAVE_MODE_EXPORT || (mode) == APP_SAVE_MODE_STEMS)

/* Whether `mode` is about a project file rather than about audio. */
#define APP_SAVE_IS_PROJECT(mode) \
  ((mode) == APP_SAVE_MODE_PROJECT_SAVE || (mode) == APP_SAVE_MODE_PROJECT_OPEN)

/*
 * A take the capture stream died in the middle of, and what it would take to
 * carry it on.
 *
 * Nothing here is at risk: the engine closes the WAV the moment the stream
 * goes, and the window turns the clip that was growing into a finished one, so
 * what was played is on disk and on the timeline whether or not the device
 * ever comes back. This is only the anchor - if it does come back, and soon
 * enough that somebody is still holding the cable, the next take can start
 * exactly where the last one stopped instead of wherever the cursor is.
 *
 * The geometry is kept because a device that returns is not necessarily the
 * device that went. One that comes back at another rate or another channel
 * count cannot be spliced onto the end of a lane recorded at the old one, and
 * doing it quietly would be worse than not doing it at all.
 */
typedef struct
{
  int waiting;    /* a take was cut short and has not been carried on yet */
  long track;     /* the lane it was on */
  uint64_t at;    /* the frame a take carrying it on should start at */
  double lost_at; /* GetTime() when the stream went, for the window below */
  unsigned rate;
  unsigned channels;
  /*
   * The file it was being written to, so the rest of it can go on the end
   * rather than into a second one beside it. Empty when there is nothing to
   * carry on.
   */
  char path[AUD_ENGINE_PATH_MAX];
} app_interrupted;

typedef struct
{
  int open;
  app_save_mode mode;
  char take[AUD_ENGINE_PATH_MAX]; /* the file as it was written */
  char folder[AUD_PATH_MAX];      /* the folder being offered, as typed */
  char name[APP_NAME_MAX];
  double seconds; /* how long the take was, kept so the engine is not re-asked */
  int focus;      /* an app_save_field */

  /*
   * What the list offers, and the strings the widget reads. Sub-folders and
   * the files the question is about - WAVs, or sessions - because a browser
   * that would not show you the file you came for is not one, and one that
   * would not show you the takes already in a folder is asking you to file
   * another one on top of them blind.
   */
  char rows[APP_MAX_FOLDERS][APP_NAME_MAX];
  char row_is_dir[APP_MAX_FOLDERS];
  const char *labels[APP_MAX_FOLDERS];
  int count;
  int scroll;
  char listed[AUD_PATH_MAX]; /* the folder `rows` was built from */
  /*
   * Whether dot files and dot folders are among them. Off by default, because
   * hardly anybody keeps recordings in one - but the folder a take has to go
   * into is not always the one anybody would have chosen, and a browser that
   * cannot reach ~/.local/share at all is a browser that has to be given up on
   * and the path typed instead.
   *
   * Part of the dialog rather than of the app: it is answered per question,
   * and the answer to "where does this take go" is not the answer to "which
   * session am I opening".
   */
  int show_hidden;

  char note[AUD_ENGINE_ERROR_MAX]; /* why the last attempt to save did not */
  /*
   * An export that was told the name is taken and asked again anyway. Only
   * exporting has this: a take being filed must never land on another take,
   * but a mix is something you write repeatedly to the same name while you get
   * it right, and refusing outright would make that impossible.
   */
  int confirmed;

  /*
   * The desktop's own chooser, while one is up. NULL the rest of the time,
   * which is most of it - see gui/chooser.h for why it is polled rather than
   * waited on, and why the built-in browser stays behind it rather than being
   * replaced by it.
   */
  struct aud_chooser *chooser;
} app_save;

/* What the edit toolbar and the edit keys both stand for. */
typedef enum
{
  APP_EDIT_UNDO = 0,
  APP_EDIT_REDO,
  APP_EDIT_CUT,
  APP_EDIT_COPY,
  APP_EDIT_PASTE,
  APP_EDIT_DELETE,
  APP_EDIT_SILENCE,
  APP_EDIT_TRIM,
  APP_EDIT_SPLIT,
  APP_EDIT_DUPLICATE,
  APP_EDIT_FADE_IN,
  APP_EDIT_FADE_OUT,
  APP_EDIT_SELECT_ALL,
  /* the selection along the timeline, by app.move_by frames */
  APP_EDIT_MOVE,
  /*
   * How loud the selection is, rather than where it is or how much of it there
   * is: a decibel at a time by hand, or measured and put on a target. After
   * the rest deliberately - the reply the ones above share is a table indexed
   * by this enum, and these two say what they did in decibels instead.
   */
  APP_EDIT_LOUDER,
  APP_EDIT_QUIETER,
  APP_EDIT_NORMALIZE_PEAK,
  APP_EDIT_NORMALIZE_LOUDNESS,
} app_edit_action;

/* What one press of the gain keys, or one click of the bar, is worth. */
#define APP_GAIN_STEP_DB 1.0

/*
 * Audio an edit has to touch before it is worth stopping to ask about.
 *
 * There is a real cost to asking. Every edit here is undoable, so a dialog in
 * front of each one would be a click bought with nothing - and a confirmation
 * that appears constantly is one nobody reads, which makes the dangerous case
 * worse rather than better. Ten seconds is about where an edit stops being a
 * tidy-up and starts being a decision.
 */
#define APP_CONFIRM_SECONDS 10.0

/* What is waiting on an answer, and what to do when one arrives. */
typedef enum
{
  APP_CONFIRM_NONE = 0,
  APP_CONFIRM_EDIT,        /* an edit action, held in `action` */
  APP_CONFIRM_CLOSE_TRACK, /* a lane, held in `track` */
  APP_CONFIRM_UNDO,        /* an undo that would strand a file on disk */
  APP_CONFIRM_APPLY,       /* the spectrum panel's Apply */
  APP_CONFIRM_QUIT,
} app_confirm_kind;

/* Reasons a question can give. More than this and it is a manual, not a dialog. */
#define APP_CONFIRM_REASONS 3
#define APP_CONFIRM_LINE 140
#define APP_CONFIRM_LABEL 24

/*
 * The question that stops an action until it is answered.
 *
 * Nothing here decides anything: it holds a question, the reasons for asking
 * it, and what was about to happen. The answer is carried out by whoever put
 * the question up - see app_confirm_take() - so the dialog itself never needs
 * to know what any of these actions mean.
 */
typedef struct
{
  int open;
  app_confirm_kind kind;
  app_edit_action action; /* read only when kind is APP_CONFIRM_EDIT */
  long track;             /* read only when kind is APP_CONFIRM_CLOSE_TRACK */

  char title[APP_CONFIRM_LINE];
  char reason[APP_CONFIRM_REASONS][APP_CONFIRM_LINE];
  int reasons;
  char accept[APP_CONFIRM_LABEL]; /* what the button that goes ahead says */
  /*
   * Whether going ahead loses something no undo will bring back. It colours
   * the button, and it is the difference between "this is a big edit" and
   * "this is the end of that piece of work".
   */
  int irreversible;
} app_confirm;

typedef struct
{
  /*
   * sizeof(app) as the code that allocated this saw it. First, deliberately:
   * it is the one field a differently built copy of the window can still find,
   * everything after it having possibly moved. A hot reload reads it to decide
   * whether the session it is being handed was laid out by a build that agrees
   * with it - see hotreload/plug.h.
   */
  size_t self_size;

  aud_engine *engine;
  aud_viz *viz;
  aud_engine_config cfg;

  /*
   * The project: what has been recorded or imported, and every edit made to it.
   * The window is a view of this and nothing else - see edit/doc.h.
   */
  aud_doc doc;
  aud_clipboard clipboard;
  aud_timeline timeline;
  aud_player player;

  /*
   * Frames APP_EDIT_MOVE would move the selection by. It is the one edit with
   * an argument, and it has to survive being asked about: the question the
   * dialog puts up is answered a frame or more later, by which time the drag
   * that asked for it is over and the keystroke is long gone.
   */
  int64_t move_by;

  /*
   * A file being auditioned from the save dialog, which is the one thing heard
   * here that is not the project. Kept beside the player rather than inside
   * the dialog because it outlives no dialog but has to be stopped by
   * whichever one closes - see preview.h.
   */
  aud_preview preview;

  /*
   * The take being recorded, as it lands on the timeline. The engine is still
   * writing the WAV; this is the same frames arriving on the track at the same
   * time, so the waveform grows while it is being played rather than appearing
   * when it is over.
   *
   * `record_track` is the lane it is going onto and `record_at` where it
   * started, both fixed when Record was pressed - the cursor may well have
   * moved since, and the take belongs where it began.
   */
  long record_track;
  /*
   * The lane the last take landed on, kept after record_track has been let go.
   * A take that is moved by the dialog has to have its block told where it
   * ended up, or a project saved afterwards would point at a file that is no
   * longer there - see edit/project.h.
   */
  long last_take_track;
  uint64_t record_at;
  /*
   * Frames of the take still to be thrown away before it starts landing on the
   * timeline. Only ever non-zero when a take begins so near the start of the
   * project that the latency correction cannot be a shift alone; see
   * take/latency.h.
   */
  uint64_t record_skip;

  /* the take the device was lost in the middle of, if there was one */
  app_interrupted interrupted;

  /*
   * Play the project while recording over it, and by how much to correct for
   * having heard it late. `latency_ms` below zero means "work it out from the
   * buffers", which is what it is until someone measures theirs.
   */
  int overdub;
  double latency_ms;

  /*
   * The rest of what the transport will do when it is next pressed. Intents
   * rather than state, like `overdub` above: the player is told them when a
   * pass starts, and holds none of them between passes.
   *
   * The tempo they are counted on is not here - it belongs to the project, is
   * saved with it, and is what the ruler draws. See edit/doc.h.
   */
  int loop;     /* Play goes round the selection instead of stopping at it */
  int click_on; /* the metronome plays over whatever else is being heard */
  float click_gain;

  /*
   * A tempo named on the command line, held until there is a document to put
   * it in - and applied after any project has opened, so `--tempo` means it
   * rather than being overwritten by what the session was saved at. Zero in
   * either is "nothing was said"; a bare pulse is one beat to the bar, which
   * click.h already treats as no bar at all.
   */
  double start_tempo;
  unsigned start_beats;
  float *take_buf; /* APP_TAKE_BUF_SAMPLES floats, interleaved */
  /* frames of those the current engine's channel count fits; 0 without one */
  size_t take_buf_frames;

  /*
   * The drawer: the visualiser, which was the whole window and is now a panel
   * of it, and the spectrum editor beside it.
   *
   * The visualiser still runs while it is shut: the analysis is cheap, and a
   * strip that has to warm up when you open it is worse than one that is
   * simply there. The spectrum editor does not - it reads the timeline rather
   * than the interface, and reading a take nobody is looking at would cost
   * real work for nothing.
   */
  app_drawer drawer;
  float viz_height;

  /* the spectrum of what is on the timeline, and what is being taken out of it */
  aud_repair_panel repair;

  /* the bottom line: what the last thing to happen was */
  char status[APP_STATUS_MAX];

  /* WAVs named on the command line, opened once there is a project to open
   * them into. These point into argv, which outlives everything here. */
  const char *open_paths[APP_MAX_OPEN];
  int open_count;

  char prefix[512];
  /*
   * Where takes are kept, from --dir or the config file. Empty means the
   * working directory. The prefix above is placed in it once, at startup, so
   * everything after that is holding a path rather than half of one.
   */
  char take_dir[AUD_PATH_MAX];
  /*
   * The project file this session is being kept in, or "" when it has never
   * been saved. `project_dirty` is whether anything has changed since it was,
   * which is what the title bar's asterisk and the save prompt read.
   *
   * Distinct from aud_doc.dirty, which means "the view has not drawn this yet"
   * and is set and cleared many times a second.
   */
  char project_path[AUD_PATH_MAX];
  int project_dirty;
  /* whether stopping a take opens the dialog that asks where it should go */
  int want_dialog;
  app_save save;
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

  /* the question waiting on an answer, over the top of even that */
  app_confirm confirm;

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
  /*
   * The capture gain, which unlike the one above reaches the take. Held here
   * rather than only in the engine because it outlives one: a device swapped
   * mid-session is the same interface into the same bass, and having to set
   * the level again because the dropdown was touched would be a bug.
   */
  float input_gain;

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

/* -- plug.c ---------------------------------------------------------------- */

/* Set the bottom line. Printf-style, because most callers have a value in it. */
void app_set_status(app *a, const char *fmt, ...) AUD_PRINTF(2, 3);

/* -- take.c ---------------------------------------------------------------- */

/*
 * Open the selected capture device and stand up the analyser behind it, or
 * close both. Returns 0 with an engine, or -1 with the reason in a->fatal.
 */
int app_open_engine(app *a);
void app_close_engine(app *a);

/* Reopen on the row the dropdown now points at, falling back to `previous`. */
void app_switch_device(app *a, int previous);

/*
 * Open the selected device again once it is back, after the window came up
 * without it or its stream died with the cable, and carry on the take it died
 * under if that is still what anybody wants. Only worth calling off the back of
 * a device list that actually changed.
 */
void app_recover_engine(app *a);

/*
 * Notice that the capture stream died under a take, which has to happen before
 * anything else in the frame looks at the timeline: the engine that was writing
 * the take is the only thing that can still be asked what it wrote.
 */
void app_check_capture_loss(app *a);

void app_begin_take(app *a);
void app_stop_take(app *a, const aud_engine_status *st);
void app_toggle_record(app *a, const aud_engine_status *st);

/*
 * Move whatever the engine has captured onto the track being recorded into.
 * Called every drawn frame, which is what makes the waveform grow as it is
 * played rather than appear when it stops.
 */
void app_pump_take(app *a);

/* Give the video encoder its slice of the frame, if one is running. */
void app_pump_render(app *a);
void app_cancel_render(app *a);

/*
 * Done with the take at `path`: put it on the timeline, and start its video if
 * one was asked for. Called when the take stops, or - with the dialog on - once
 * it has been answered, because both the track and ffmpeg should read the WAV
 * where it ended up rather than where it happened to be written.
 */
void app_finish_take(app *a, const char *path);

/* -- actions.c ------------------------------------------------------------- */

/* Read a WAV into the project as a new track. Says how it went on the status. */
void app_load_track(app *a, const char *path);

/*
 * Carry one out, and say on the status line what happened.
 *
 * Asks first when the edit is big enough or lossy enough to be worth asking
 * about - see confirm.c - in which case nothing happens until the question is
 * answered. app_edit_now() is the same thing with the asking already done.
 */
void app_edit(app *a, app_edit_action action);
void app_edit_now(app *a, app_edit_action action);

/* Move the selection along the timeline by `by` frames; see APP_EDIT_MOVE. */
void app_move_selection(app *a, int64_t by);

/* Play from the cursor, or from the start of the selection. Stops if playing. */
void app_toggle_play(app *a);

/*
 * Hand the player the transport's options: the tempo to count on, whether the
 * metronome plays, and whether the pass goes round. Called before a pass
 * starts and again whenever one of them changes, so a tempo nudged while
 * something is playing takes without stopping it.
 */
void app_apply_transport(app *a);

/* Change the tempo by `beats`, holding it to what the metronome will play. */
void app_nudge_tempo(app *a, double beats);

/* Mix the project - or the selection - down to a WAV. */
void app_export(app *a, const char *path);

/*
 * The same range, written as one WAV a track rather than one mix. `path` names
 * the set: every file is that name with a track's number and name on it. See
 * edit/export.h for what the set is worth, which is that it adds back up to
 * what app_export() would have written.
 */
void app_export_stems(app *a, const char *path);

/* -- save.c ---------------------------------------------------------------- */

/* Ask where the take at `path`, `seconds` long, should be kept. */
void app_save_open(app *a, const char *path, double seconds);

/* The same browser, asking which WAV to bring in rather than where to put one. */
void app_open_dialog(app *a);

/*
 * ...and asking where the mixed-down project should be written. With `stems`
 * it asks for the set of one-WAV-a-track instead, which is the same question
 * about the same range and so the same dialog.
 */
void app_export_dialog(app *a, int stems);

/*
 * Write the session out. With a project file already named this writes it
 * straight back; without one it asks, which is what "Save as" always does.
 */
void app_save_project(app *a);
void app_save_project_as(app *a);

/* Open a session, replacing whatever is on the timeline. */
void app_open_project_dialog(app *a);

/*
 * Draw the dialog and carry out what was clicked. Called from the drawing, over
 * the top of everything else, and only while a->save.open.
 */
void app_save_draw(app *a);

/* Answer it as if "Keep here" had been pressed. Escape, and the Cancel button. */
void app_save_dismiss(app *a);

/* Drop the dialog without answering it, for the way out of the program. */
void app_save_shutdown(app *a);

/* -- confirm.c ------------------------------------------------------------- */

/*
 * Ask about `action` if it is worth asking about, and return non-zero when a
 * question went up - in which case the caller has done nothing and must not.
 * Zero means carry on: either there was nothing to ask, or the edit is one
 * that changes nothing.
 */
int app_confirm_edit(app *a, app_edit_action action);

/* The same for the ones that are not edit actions. */
int app_confirm_close_track(app *a, size_t index);
int app_confirm_undo(app *a);
int app_confirm_apply(app *a, double seconds, const char *track);
int app_confirm_quit(app *a);

/*
 * Draw the question and carry out whatever was answered. Called from the
 * drawing, over the top of everything else including the save dialog.
 *
 * Returns non-zero when the answer was to quit, which is the one answer the
 * dialog cannot carry out itself.
 */
int app_confirm_draw(app *a);

/* Take the question down unanswered, for the paths that give up on it. */
void app_confirm_dismiss(app *a);

/* -- screen.c -------------------------------------------------------------- */

/*
 * Draw the whole window and carry out whatever was clicked. Returns non-zero
 * when the answer to a question was to quit, which is the one thing the
 * drawing cannot do for itself - the run loop is the shell's. See main.c.
 */
int app_draw_frame(app *a, const aud_engine_status *st);

/* The window with no engine behind it: a message, and the picker to escape by. */
int app_draw_fatal(app *a);

#endif /* AUDIAKI_GUI_APP_H */
