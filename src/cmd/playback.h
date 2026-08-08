/* SPDX-License-Identifier: MIT */
/*
 * playback.h - what the person recording hears while they record.
 *
 * Two things can come out of it, either or both: the input as it is being
 * captured (--monitor), and a metronome (--click). One output carries them
 * because they are heard together, in the same headphones, at the same time.
 *
 * Apart from record.c because it is the opposite concern: nothing here is
 * allowed to end a take. An output that will not open, or that fails halfway
 * through, costs what would have been heard and leaves the recording running -
 * the file is the product, the playback is the convenience, and the convenience
 * is what gives way. None of these calls can fail in a way the caller has to
 * handle, which is that rule expressed as an interface.
 */
#ifndef AUDIAKI_CMD_PLAYBACK_H
#define AUDIAKI_CMD_PLAYBACK_H

#include "audio/click.h"
#include "backend/device.h"
#include "backend/monitor.h"

typedef struct
{
  int input;            /* mix the captured audio in, i.e. --monitor */
  const char *device;   /* output to play through; NULL means the default one */
  float gain;           /* scales the input alone - not the click, never the file */
  double click_bpm;     /* 0 for no metronome, which is the default */
  unsigned click_beats; /* beats to a bar; the first of each is accented */
  float click_gain;
} aud_playback_config;

typedef struct
{
  aud_monitor *mon; /* NULL whenever nothing is being played, including on failure */
  float *buf;       /* period_frames * channels, interleaved */
  int input;
  float gain;
  int clicking;
  aud_click click;
  unsigned long dropped;
} aud_playback;

/* Open the output, if anything was asked to come out of it. Cannot fail. */
void aud_playback_start(aud_playback *pb, const aud_device *dev,
                        const aud_playback_config *cfg);

/* Idempotent, so the cleanup path can run it whichever way the take ended. */
void aud_playback_stop(aud_playback *pb);

/* Hand a captured period to the output, mixing in the click if there is one. */
void aud_playback_feed(aud_playback *pb, const unsigned char *hw_buf, size_t frames,
                       const aud_device *dev);

#endif /* AUDIAKI_CMD_PLAYBACK_H */
