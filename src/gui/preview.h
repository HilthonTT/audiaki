/* SPDX-License-Identifier: MIT */
/*
 * preview.h - hearing a file without putting it on the timeline.
 *
 * The dialog that asks where a finished take should go used to ask it about a
 * file nobody had heard. This is the answer to that: a WAV played straight off
 * disk, through an output of its own, with nothing in the project touched.
 *
 * Deliberately not aud_player, which plays the document and knows about mixing,
 * looping and the metronome. What is being auditioned here is a file - the take
 * that just stopped, or one about to be imported - and it may well be at a rate
 * or a channel count the project is not. Loading it in to hear it would put an
 * undo step and a track's worth of memory behind a question that has not been
 * answered yet, and answering "no" would then mean taking both back.
 *
 * Pumped from the drawing loop the way aud_player is, and for the same reason:
 * the output says how much it will take, so a frame's worth is handed over per
 * frame and the output's own consumption is the clock. Nothing here has a
 * thread.
 */
#ifndef AUDIAKI_GUI_PREVIEW_H
#define AUDIAKI_GUI_PREVIEW_H

#include "media/wav.h"
#include "util/path.h"

/* Frames handed to the output at a time; it rarely wants more than this. */
#define AUD_PREVIEW_CHUNK 4096u

typedef struct aud_monitor aud_monitor;

typedef struct
{
  aud_monitor *out;
  wav_reader wav;
  int open;    /* `wav` holds a file that must be closed */
  int playing; /* ...and it is being fed to an output */
  float *buf;
  unsigned latency; /* frames the output holds when it is full */
  uint64_t written; /* frames handed over, for how far in it has got */

  /* what is being played, so the button can say so */
  char path[AUD_PATH_MAX];
} aud_preview;

void aud_preview_init(aud_preview *p);

/*
 * Play `path` from the beginning through `device` (NULL for the default).
 * Whatever was being previewed is dropped first, so pressing play on a second
 * file does the obvious thing.
 *
 * Returns 0, or -1 after saying why through log.h. A failure is not fatal to
 * anything: the file is still where it was and the dialog still works, which is
 * why the caller is only ever told so it can put the reason on the line.
 */
int aud_preview_start(aud_preview *p, const char *path, const char *device);

/* Stop and let both the output and the file go. Safe when nothing is playing. */
void aud_preview_stop(aud_preview *p);

/*
 * Hand the output as much as it will take. Called once per drawn frame.
 *
 * Returns non-zero on the frame the file ran out and playback stopped itself,
 * which is the caller's cue to put the button back.
 */
int aud_preview_pump(aud_preview *p);

int aud_preview_playing(const aud_preview *p);

/*
 * Where it has reached and how long it is, in seconds, for the readout beside
 * the button. Both are zero when nothing is playing. The position allows for
 * what the output has not played yet, the way the timeline's playhead does.
 */
double aud_preview_position(const aud_preview *p);
double aud_preview_length(const aud_preview *p);

/* The file being played, or "" when none is. */
const char *aud_preview_path(const aud_preview *p);

#endif /* AUDIAKI_GUI_PREVIEW_H */
