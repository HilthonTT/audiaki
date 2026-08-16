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

#include "audio/click.h"
#include "audio/loudness.h"
#include "edit/doc.h"
#include "edit/mix.h"

/* Frames mixed per pass; the output rarely wants more than this at once. */
#define AUD_PLAYER_CHUNK 4096u

/*
 * A `to` that means "until something stops it". What a take runs against: the
 * metronome has to keep counting past the end of what is on the timeline,
 * because the whole point of recording to one is that there is nothing there
 * yet.
 */
#define AUD_PLAYER_OPEN_ENDED UINT64_MAX

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

  /*
   * Round and round [from, to) rather than stopping at it. Ignored for an
   * open-ended pass, which has no end to come back from.
   */
  int looping;

  /*
   * Whether the project itself is heard. Off leaves the metronome alone with
   * the stream, which is what recording with Overdub turned off and the click
   * turned on asks for: count me in, but do not play me what is already there.
   */
  int mix_on;

  /*
   * The metronome, mixed over the project on its way out and into nothing
   * else - the take is written from what the interface delivered, and a click
   * you can hear on the recording is a click in the wrong place.
   *
   * Counted on the absolute project frame rather than on how long this pass
   * has been running, so beat n is where the ruler draws it whatever the
   * transport did to get there: seek into the middle of a session and the bar
   * lines and the clicks still agree.
   */
  aud_click click;
  int click_on;
  /*
   * What the click was asked for, kept apart from the generator above because
   * a tempo can be set before there is a stream to play it through: the rate
   * only arrives with the output, and these are what it is built from then.
   */
  double click_bpm; /* 0 when the metronome is off */
  unsigned click_beats;
  unsigned click_subdiv;
  float click_gain;

  /*
   * How loud the mix has been since this pass started, or NULL when the
   * project's rate is not one BS.1770 can be derived at. Fed the mix and not
   * the click, for the same reason the take is not written with the click in
   * it: the metronome is something you hear, not something the project holds,
   * and a figure that counted it would not be the figure the export gets.
   */
  aud_loudness *loud;
} aud_player;

void aud_player_init(aud_player *p);

/*
 * Arm the metronome for the next pass, and for the one in progress. `bpm` of
 * zero turns it off; anything else is taken as read from the document, whose
 * tempo it is. Gain is peak amplitude, as click.h has it.
 *
 * `subdiv` is ticks to a beat, and is the document's grid division: the lines
 * on the ruler and the ticks in the headphones are the same grid, so a session
 * snapping to thirds is counted out in thirds.
 *
 * Settable while playing so a tempo can be found by ear against what is
 * already there, which is how anybody actually arrives at one.
 */
void aud_player_set_click(aud_player *p, double bpm, unsigned beats_per_bar,
                          unsigned subdiv, float gain);

/* Whether the next pass, or this one, goes round rather than stopping. */
void aud_player_set_loop(aud_player *p, int on);

/*
 * How loud what is playing has been, for the meter beside the transport. Fills
 * `out` with nothing measured when nothing is playing, so a caller needs no
 * second path for that - see audio/loudness.h.
 *
 * It reads what has been handed to the output rather than what has come out of
 * it, so it runs up to one buffer ahead of the sound. That is a few tens of
 * milliseconds against a window of four hundred, and the alternative - holding
 * the numbers back by a latency the output does not report exactly - would be
 * a meter that lied about a different thing.
 *
 * A looping pass keeps measuring rather than starting again each time round.
 * The integrated figure is a gated mean, so a passage played five times reads
 * the same as the same passage played once, which is the answer that matches
 * what would be exported.
 */
void aud_player_loudness(const aud_player *p, aud_loudness_live *out);

/* Whether the project is heard at all. On until something says otherwise. */
void aud_player_set_mix(aud_player *p, int on);

/*
 * Start playing `d` from `from` until `to`, through `device` (NULL for the
 * default). `to` of 0 plays to the end of the project, and
 * AUD_PLAYER_OPEN_ENDED runs until it is stopped.
 *
 * An open-ended pass is allowed to start with nothing to play - a metronome
 * over an empty timeline is a stream of silence with clicks in it, and that is
 * exactly what counting a take in wants.
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
 * itself, which is the caller's cue to put the transport back. A looping or
 * open-ended pass never reaches one, so it only ever returns zero.
 */
int aud_player_pump(aud_player *p, const aud_doc *d);

/*
 * Where playback has reached, allowing for what the output has not played yet.
 * Inside [from, to) whatever the pass has done: a loop reports where round it
 * is rather than how far it has been.
 */
uint64_t aud_player_head(const aud_player *p);

int aud_player_playing(const aud_player *p);

#endif /* AUDIAKI_GUI_PLAYER_H */
