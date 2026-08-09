/* SPDX-License-Identifier: MIT */
/*
 * click.h - the metronome: a beat grid mixed into what you are listening to.
 *
 * A click is not recorded. It is mixed into the playback stream on its way to
 * your headphones and nowhere else, for the same reason --monitor-gain does not
 * touch the file: the take is written from the samples the device delivered,
 * whatever was being played to the person making it.
 *
 * The grid is a pure function of the absolute frame index, not an accumulator
 * ticked once per period, so beat 400 lands on the frame the arithmetic says it
 * does whether the periods before it arrived early, late or not at all. Since
 * the frames counted are the frames captured, the beat grid is the capture
 * clock: at 120 BPM and 48 kHz, beat n is at frame n * 24000 of the file.
 *
 * Free of any audio system and of the clock, so it builds and is tested
 * anywhere - see the layout rule in DESIGN.md.
 */
#ifndef AUDIAKI_CLICK_H
#define AUDIAKI_CLICK_H

#include <stddef.h>
#include <stdint.h>

/*
 * Tempo bounds. Under 20 BPM the beats are three seconds apart and there is
 * nothing left to play along to; over 300 they are close enough together that
 * the next burst starts before the ear has finished the last.
 */
#define AUD_CLICK_BPM_MIN 20.0
#define AUD_CLICK_BPM_MAX 300.0

/*
 * Beats to a bar, deciding which one is accented. 0 and 1 both mean every beat
 * sounds alike, which is what a bare pulse is.
 */
#define AUD_CLICK_BEATS_MAX 32u
#define AUD_CLICK_DEFAULT_BEATS 4u

/* Silence to +6 dB, the range --monitor-gain covers, and half scale by default:
 * loud enough to hear over an instrument, quiet enough to leave room for one. */
#define AUD_CLICK_GAIN_MIN 0.0
#define AUD_CLICK_GAIN_MAX 2.0
#define AUD_CLICK_DEFAULT_GAIN 0.5

typedef struct
{
  double bpm;
  unsigned beats_per_bar; /* the first beat of each bar is accented */
  unsigned rate;          /* the capture rate; the grid is counted in its frames */
  float gain;             /* peak amplitude of a beat, before the accent */
} aud_click_config;

/* Opaque in practice: built by aud_click_init(), advanced by aud_click_mix(). */
typedef struct
{
  double inv_rate;
  double omega_accent; /* radians per frame */
  double omega_beat;
  double decay;   /* envelope exponent per frame */
  double spacing; /* frames per beat, as a real number */
  unsigned beats_per_bar;
  unsigned burst_frames;
  float gain;
  uint64_t frame; /* absolute index of the next frame to be mixed */
  uint64_t beat;  /* the beat whose burst is current, or the next one due */
} aud_click;

/* Fill `cfg` with the defaults for `bpm` at `rate`. */
void aud_click_config_defaults(aud_click_config *cfg, double bpm, unsigned rate);

/*
 * Prepare `c`, starting the grid at frame 0 so the first beat lands on the
 * first frame handed over. Returns 0, or -1 if the tempo or rate is outside
 * what click.h accepts.
 */
int aud_click_init(aud_click *c, const aud_click_config *cfg);

/* Put the grid back to beat one at frame zero. */
void aud_click_reset(aud_click *c);

/*
 * Move to `frame` on the same grid: the next frame mixed is that one, and the
 * beat it falls in is the beat that sounds.
 *
 * What lets a click follow a transport that jumps. The grid is counted from
 * frame zero whatever the playhead does, so seeking to the middle of a project
 * gives the beat the ruler draws there rather than a new beat one - and a loop
 * that wraps stays in time rather than restarting the bar wherever it landed.
 */
void aud_click_seek(aud_click *c, uint64_t frame);

/*
 * Add `frames` of click to `interleaved`, which holds `channels` samples per
 * frame. Adds rather than overwrites, so it layers over monitored input; a
 * caller with nothing to layer over passes a zeroed buffer.
 *
 * The same beat goes to every channel. Values are left unclipped, because the
 * playback stream clips on the way out anyway and clipping twice would be the
 * only difference.
 */
void aud_click_mix(aud_click *c, float *interleaved, size_t frames, unsigned channels);

/* Frames between beats, rounded, for anything that wants to report the grid. */
uint64_t aud_click_beat_frames(const aud_click *c);

#endif /* AUDIAKI_CLICK_H */
