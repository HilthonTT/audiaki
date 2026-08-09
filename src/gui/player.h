/* SPDX-License-Identifier: MIT */
/*
 * player.h - hearing the timeline.
 *
 * No thread of its own, unlike the capture engine, and for a reason: the output
 * says how much it will take (aud_monitor_space), so the drawing loop can hand
 * it exactly that much every frame and the output's own consumption is the
 * clock. cmd/play.c plays a file back the same way.
 *
 * What that buys is that the project is only ever touched by one thread. An
 * edit made while playback is running cannot race the mix, because the mix
 * happens between two edits rather than beside them - and an editor where
 * cutting during playback was a data race would be an editor that crashed.
 */
#ifndef AUDIAKI_GUI_PLAYER_H
#define AUDIAKI_GUI_PLAYER_H

#include "edit/doc.h"
#include "edit/mix.h"

/* Frames mixed per pass; the output rarely wants more than this at once. */
#define AUD_PLAYER_CHUNK 4096u

typedef struct aud_monitor aud_monitor;

typedef struct
{
  aud_monitor *out;
  aud_mixer mix;
  float *buf;

  unsigned rate;
  unsigned channels;
  unsigned latency; /* frames the output holds when it is full */

  uint64_t from;    /* where this pass started */
  uint64_t to;      /* where it stops */
  uint64_t written; /* frames handed over since it started */
  int playing;
} aud_player;

void aud_player_init(aud_player *p);

/*
 * Start playing `d` from `from` until `to`, through `device` (NULL for the
 * default). `to` of 0 plays to the end of the project.
 *
 * Returns 0, or -1 after saying why through log.h - an output that will not
 * open is not fatal to anything, and the caller carries on without it.
 */
int aud_player_start(aud_player *p, const aud_doc *d, uint64_t from, uint64_t to,
                     const char *device);

/* Stop, and let the output go. Safe when nothing is playing. */
void aud_player_stop(aud_player *p);

/*
 * Hand the output as much as it will take. Called once per drawn frame; it is
 * what keeps playback fed, so a window that stops drawing stops playing.
 *
 * Returns non-zero once the end has been reached and playback has stopped
 * itself, which is the caller's cue to put the transport back.
 */
int aud_player_pump(aud_player *p, const aud_doc *d);

/* Where playback has reached, allowing for what the output has not played yet. */
uint64_t aud_player_head(const aud_player *p);

int aud_player_playing(const aud_player *p);

#endif /* AUDIAKI_GUI_PLAYER_H */
