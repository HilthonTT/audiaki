/* SPDX-License-Identifier: MIT */
/*
 * info.h - measure a finished take.
 *
 * The live meter tells you what is happening now; this tells you what you
 * ended up with. Was anything clipped, how much headroom is left, how quiet is
 * the room between the notes - the questions you ask after playing, not while.
 *
 * Reads through wav.h and touches no ALSA, so it can be unit tested anywhere.
 */
#ifndef AUDIAKI_INFO_H
#define AUDIAKI_INFO_H

#include "take/meta.h"

#include <stdint.h>
#include <stdio.h>

/* the reader already refuses anything wider, so this cannot be overflowed */
#define AUD_INFO_MAX_CHANNELS 64u

/*
 * Levels are normalised to full scale, not decibels: 1.0 is a sample at the
 * top of the range. aud_format_dbfs() converts, and the printers do.
 */
typedef struct
{
  uint32_t rate;
  uint16_t channels;
  uint16_t bits;
  int is_float;

  uint64_t frames;        /* frames actually readable */
  uint64_t header_frames; /* frames the data chunk claims */
  double duration;        /* seconds of readable audio */

  double peak;        /* loudest sample in the take */
  double rms;         /* over every sample of every channel */
  double noise_floor; /* the level of a quiet moment; see aud_info_analyse */
  uint64_t clipped;   /* samples sitting at or beyond full scale */
  uint64_t samples;   /* frames * channels, the denominator behind rms */

  double channel_peak[AUD_INFO_MAX_CHANNELS];
  double channel_rms[AUD_INFO_MAX_CHANNELS];
  double channel_dc[AUD_INFO_MAX_CHANNELS]; /* mean sample value */

  /* what the file says about itself, if anything; see meta.h */
  aud_meta_info meta;
} aud_info_report;

/*
 * Read `path` from end to end and fill `out`.
 *
 * The noise floor is the tenth percentile of the RMS of short windows, which
 * is a fair description of "the room with nothing being played" and survives a
 * take that never actually goes quiet. A file containing digital silence
 * reports a floor of zero, because that is the truth about it.
 *
 * Returns 0 on success, or -1 after reporting the reason through log.h.
 */
int aud_info_analyse(const char *path, aud_info_report *out);

/* Human readable, one field per line. */
void aud_info_print(FILE *out, const char *path, const aud_info_report *r);

/* The same numbers as a single JSON object, for --json. */
void aud_info_print_json(FILE *out, const char *path, const aud_info_report *r);

/*
 * One take per line instead, for the several files --info accepts at once.
 * Twelve takes down a session, the question is which of them clipped and which
 * came out too quiet - not the full report on each, twelve times over.
 *
 * aud_info_print_row_header() writes the column headings once, then a row per
 * file. `width` is how much room to give the name column; pass the longest name
 * about to be printed, and aud_info_row_width() will bound it sensibly.
 */
unsigned aud_info_row_width(unsigned longest_name);
void aud_info_print_row_header(FILE *out, unsigned width);
void aud_info_print_row(FILE *out, const char *path, const aud_info_report *r,
                        unsigned width);

#endif /* AUDIAKI_INFO_H */
