/* SPDX-License-Identifier: MIT */
/*
 * engine.h - the desktop app's capture thread and transport.
 *
 * The CLI recorder owns its own control flow: aud_recorder_run() blocks until
 * the take is over. A window cannot do that - it has to keep drawing at 60 Hz
 * while the audio keeps arriving at whatever rate the interface delivers it -
 * so the engine runs the capture loop on its own thread and the UI talks to it
 * through this small transport interface.
 *
 * The capture stream is opened when the engine is created and stays open until
 * it is destroyed, independent of whether a take is being written. That is what
 * lets the visualiser and the level meter run before you press record, which is
 * how you set your input gain in the first place.
 *
 * Threading contract: every function here except aud_engine_read_visual() is
 * safe to call from the UI thread while capture runs. aud_engine_read_visual()
 * is the single consumer of the visualiser ring and must only ever be called
 * from one thread - in practice, the one that draws.
 */
#ifndef AUDIAKI_GUI_ENGINE_H
#define AUDIAKI_GUI_ENGINE_H

#include "format.h"

#include <stddef.h>
#include <stdint.h>

/* Long enough for any path a file dialog or a take name will produce. */
#define AUD_ENGINE_PATH_MAX 1024u

/* Longest failure description carried back to the UI. */
#define AUD_ENGINE_ERROR_MAX 256u

typedef enum
{
  AUD_ENGINE_IDLE = 0, /* capturing for the meters, writing nothing */
  AUD_ENGINE_RECORDING,
  AUD_ENGINE_PAUSED, /* still capturing, not appending to the file */
  AUD_ENGINE_FAILED, /* the capture stream died; the take was salvaged */
} aud_engine_state;

typedef struct
{
  const char *device;     /* ALSA capture device */
  unsigned rate;          /* requested; check aud_engine_rate() for the result */
  unsigned channels;      /* requested */
  aud_format format;      /* AUD_FORMAT_UNKNOWN negotiates */
  unsigned period_frames; /* smaller means a livelier display and more xruns */
  unsigned periods;
  const char *monitor_device; /* ALSA playback device; NULL means the default */
  /*
   * Seconds of audio to keep while idle, so a take can begin before the button
   * was pressed. 0 starts every take at the press.
   */
  double preroll;
} aud_engine_config;

typedef struct aud_engine aud_engine;

/*
 * A snapshot of what the engine is doing. Taken under the engine's lock and
 * handed back by value, so the UI can read it field by field without anything
 * changing underneath it mid-frame.
 */
typedef struct
{
  aud_engine_state state;
  double elapsed; /* seconds committed to the file; frozen while paused */
  double peak;    /* most recent period's peak, 0.0 to 1.0 */
  uint64_t frames;
  uint64_t bytes;
  unsigned xruns;
  int clipped;                   /* a sample hit full scale during this take */
  int monitoring;                /* playback is actually open, not just wanted */
  unsigned long monitor_dropped; /* frames the output could not keep up with */
  double preroll_held;           /* seconds a take pressed now would start with */
  double preroll_size;           /* seconds it holds once full; 0 when disabled */
  char path[AUD_ENGINE_PATH_MAX];
  char error[AUD_ENGINE_ERROR_MAX]; /* empty unless something went wrong */
} aud_engine_status;

/* Fill `cfg` with the same defaults the CLI recorder uses. */
void aud_engine_config_defaults(aud_engine_config *cfg);

/*
 * Open the capture device and start the capture thread. Returns NULL after
 * reporting the reason through log.h.
 */
aud_engine *aud_engine_create(const aud_engine_config *cfg);

/*
 * Stop the thread, finalise any take still in progress and close the device.
 * Safe on NULL.
 */
void aud_engine_destroy(aud_engine *e);

/* What the device actually negotiated, which may not be what was asked for. */
unsigned aud_engine_rate(const aud_engine *e);
unsigned aud_engine_channels(const aud_engine *e);
aud_format aud_engine_format(const aud_engine *e);

/*
 * Begin writing a take to `path`. Refuses if a take is already open, or if the
 * file exists and `overwrite` is zero. Returns 0 on success, -1 with the reason
 * in the next status snapshot.
 *
 * With a pre-roll configured the take opens with the seconds already captured
 * while idle, so it starts before this call rather than at it.
 */
int aud_engine_start(aud_engine *e, const char *path, int overwrite);

/*
 * Stop appending to the file without closing it. The stream keeps running, so
 * the meters and the visualiser stay live. A no-op unless recording.
 */
void aud_engine_pause(aud_engine *e);

/* Continue appending to the same file. A no-op unless paused. */
void aud_engine_resume(aud_engine *e);

/*
 * Finalise the take: patch the WAV header and close the file. Returns 0 on
 * success, -1 if the file could not be closed cleanly. A no-op when idle.
 */
int aud_engine_stop(aud_engine *e);

/* Copy the current state into `out`. */
void aud_engine_status_get(aud_engine *e, aud_engine_status *out);

/*
 * Ask for playback monitoring. The stream is opened and closed on the capture
 * thread, so this only records the intent - watch status.monitoring to see
 * whether it actually came up.
 */
void aud_engine_set_monitor(aud_engine *e, int enabled);

/* Monitoring level, clamped to [0.0, 2.0] so a quiet take can be pushed. */
void aud_engine_set_monitor_gain(aud_engine *e, float gain);

int aud_engine_monitor_wanted(const aud_engine *e);

/*
 * Drain up to `max` mono samples of recently captured audio for the display.
 * Returns how many were copied, which is zero when the capture thread has not
 * produced anything since the last call. Single consumer only.
 */
size_t aud_engine_read_visual(aud_engine *e, float *mono, size_t max);

#endif /* AUDIAKI_GUI_ENGINE_H */
