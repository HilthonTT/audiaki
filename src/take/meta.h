/* SPDX-License-Identifier: MIT */
/*
 * meta.h - what a take says about itself.
 *
 * A WAV file with nothing but a fmt chunk is a filename and a modification
 * date, and both are lost the first time it is copied somewhere. So audiaki
 * writes two standard chunks ahead of the audio:
 *
 *   LIST/INFO  the tags every tagger and most players already read - the
 *              software, the date, the note, the device it came from
 *   bext       the Broadcast Wave extension (EBU Tech 3285): origination date
 *              and time, where the take falls within the day, and the signal
 *              chain as a coding history line
 *
 * Both are written before the data chunk. That is where the BWF specification
 * requires bext, and it means an interrupted take carries its metadata even
 * though the sizes in the header have not been patched yet.
 *
 * Free of any audio system, and of the clock as well: aud_meta_build() is a
 * pure function of what it is given, and asking the system what time it is
 * happens separately in aud_meta_stamp_now(). That is what makes the chunk
 * layout testable.
 */
#ifndef AUDIAKI_META_H
#define AUDIAKI_META_H

#include <stddef.h>
#include <stdint.h>

/*
 * Enough for both chunks with every field at its longest: bext is 610 bytes
 * plus a coding history line, and LIST/INFO is four tags of bounded length.
 * aud_meta_build() writes nothing it cannot fit, so this is a ceiling rather
 * than a promise about any particular take.
 */
#define AUD_META_MAX_BYTES 2048u

/* Bounds on what a caller may hand over. Longer values are truncated. */
#define AUD_META_NOTE_MAX 200u
#define AUD_META_DEVICE_MAX 127u

/* What to write. Zeroed fields are simply left out of the chunks. */
typedef struct
{
  const char *note;     /* free text from --note; NULL for none */
  const char *device;   /* the capture device the take came from */
  const char *software; /* what wrote the file; NULL means audiaki and version */

  /* the stream as written, for the coding history line */
  unsigned rate;
  unsigned channels;
  unsigned bits;

  /* local wall clock when the take started; a zero year leaves the date out */
  unsigned year;
  unsigned month;
  unsigned day;
  unsigned hour;
  unsigned minute;
  unsigned second;

  /*
   * Samples from local midnight to the first frame of the take - bext's
   * TimeReference, the field that lets two takes from one session line up on a
   * timeline without either carrying timecode.
   */
  uint64_t time_reference;
} aud_meta;

/* Fill `m` with the defaults: audiaki as the software, and no clock. */
void aud_meta_defaults(aud_meta *m);

/*
 * Set the date, time and time reference from the system clock in local time.
 * `rate` is the sample rate the time reference is counted in. Leaves the clock
 * fields at zero if the host has no usable local time, which drops the date
 * from the chunks rather than writing a wrong one.
 */
void aud_meta_stamp_now(aud_meta *m, unsigned rate);

/*
 * Serialise the chunks into `out`, returning how many bytes were written, or 0
 * when there was nothing worth writing or no room to write it in. The result
 * is a whole number of complete, word aligned RIFF chunks, so a caller can
 * drop it straight into a file between the fmt and data chunks.
 */
size_t aud_meta_build(const aud_meta *m, unsigned char *out, size_t size);

/* -- reading back ---------------------------------------------------------- */

/*
 * What was found in a file. Empty strings mean the field was absent; `present`
 * is non-zero once anything at all has been read, which is what a printer
 * needs to decide whether to say anything.
 */
typedef struct
{
  int present;
  char note[AUD_META_NOTE_MAX + 1];
  char software[64];
  char device[AUD_META_DEVICE_MAX + 1];
  char recorded[24]; /* "YYYY-MM-DD HH:MM:SS", or as much of it as was there */
  char originator[33];
  char coding_history[192];
} aud_meta_info;

/*
 * Parse the body of a LIST chunk (including its four byte form type) or of a
 * bext chunk into `out`, adding to whatever is already there. Both tolerate
 * truncation and neither trusts a length field: a malformed chunk yields
 * fewer fields, never a read outside `bytes`.
 */
void aud_meta_read_list(aud_meta_info *out, const unsigned char *body, size_t bytes);
void aud_meta_read_bext(aud_meta_info *out, const unsigned char *body, size_t bytes);

#endif /* AUDIAKI_META_H */
