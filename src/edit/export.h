/* SPDX-License-Identifier: MIT */
/*
 * export.h - the project, back out as a WAV.
 *
 * What every edit has been leading to. It writes the same mix playback plays -
 * see mix.h - so what comes out of the file is what came out of the speakers,
 * which is the one guarantee an editor owes anybody.
 *
 * Written through the same streaming writer the recorder uses, a block at a
 * time, so exporting an hour costs an hour's worth of disk and a period's worth
 * of memory rather than both at once.
 *
 * Two ways out, and they are the same way twice: the mixdown, and the stems -
 * one file per lane, each holding that lane alone. The stems exist because a
 * mixdown is the end of the road. Handing somebody a session means handing them
 * something they can still change, and a folder of WAVs that line up is the one
 * thing every other program can open. See aud_export_stems() for the property
 * that makes them worth exporting at all.
 */
#ifndef AUDIAKI_EDIT_EXPORT_H
#define AUDIAKI_EDIT_EXPORT_H

#include "edit/doc.h"

#include <stdint.h>

/* What an export is asked for. */
typedef struct
{
  const char *path;
  unsigned channels; /* 1 or 2; 0 takes the widest track in the project */
  unsigned bits;     /* 16, 24 or 32; 0 means 24, which is what takes are */
  int overwrite;
  /*
   * The range to write. `to` of 0 means "to the end of the project", so the
   * common case - all of it - is two zeros rather than a length that has to be
   * worked out first.
   */
  uint64_t from;
  uint64_t to;
} aud_export_options;

/* Fill `opts` with the defaults described above. */
void aud_export_defaults(aud_export_options *opts);

/*
 * Mix `d` down and write it. Returns 0 on success, or -1 with `*why` set to a
 * static description. `why` may be NULL.
 *
 * A failure leaves no file behind: a half-written export is worse than none,
 * because it looks like a finished one in a directory listing.
 */
int aud_export_wav(const aud_doc *d, const aud_export_options *opts, const char **why);

/* -- stems ------------------------------------------------------------------ */

/*
 * How much of a track's name a stem's filename carries. Long enough to tell
 * "Rhythm" from "Rhythm double", short enough that sixty-four of them in one
 * folder are still readable.
 */
#define AUD_EXPORT_STEM_NAME_MAX 48u

/*
 * Whether track `index` would be written as a stem: it has audio on it, and it
 * is one the mix can hear.
 *
 * Exposed so a caller can work out the whole set of filenames before any of
 * them is written - which is what --force is checked against, and what the
 * window counts to say how many files it is about to leave in a folder.
 */
int aud_export_is_stem(const aud_doc *d, size_t index);

/*
 * Where stem `index` of a set based at `base` is written: the base without its
 * extension, then the track's number and name, then `.wav`. Exporting to
 * `~/Takes/song.wav` gives `~/Takes/song-01-Rhythm.wav`, `song-02-Lead.wav`
 * and so on.
 *
 * Numbered by the track's place in the project rather than by how many stems
 * came before it, so a stem's number is the lane you can see - and a gap in
 * the numbers is a muted lane, said out loud rather than hidden.
 *
 * `name` is reduced to letters, digits, dashes and underscores: a track called
 * "Gtr / DI" must not export into a folder called Gtr, and a name that leaves
 * nothing usable behind gives the number on its own. Two tracks of the same
 * name are still two files, because the number is in front of it.
 *
 * Returns 0 on success, -1 with `dst` untouched when the result would not fit.
 */
int aud_export_stem_path(char *dst, size_t size, const char *base, size_t index,
                         const char *name);

/*
 * Write every audible track of `d` as its own WAV, named by the rule above,
 * and report how many through `written` (which may be NULL).
 *
 * The stems add back up to the mixdown, sample for sample: they run over the
 * same range, at the same rate, depth and channel count, and each carries the
 * gain and pan that track sits in the mix with - see aud_mix_read_track(). So
 * they can be dropped into anything else, lined up at the start, and the sum is
 * the mix that came out of here. That property is the point of the feature, and
 * it is what the test asserts.
 *
 * `opts->path` is the base name rather than a file that gets written; `from`,
 * `to`, `bits`, `channels` and `overwrite` mean what they do for a mixdown, and
 * the range is resolved once for the set so the files line up.
 *
 * Muted tracks, tracks silenced by another track's solo, and empty lanes are
 * not written. A project where that leaves nothing is an error rather than a
 * folder of no files.
 *
 * Returns 0 on success, or -1 with `*why` set. A failure part way through takes
 * the stems it had already written back out again: the set is what was asked
 * for, and half of one is worse than none, because the half looks complete in a
 * directory listing.
 */
int aud_export_stems(const aud_doc *d, const aud_export_options *opts, size_t *written,
                     const char **why);

#endif /* AUDIAKI_EDIT_EXPORT_H */
