/* SPDX-License-Identifier: MIT */
/*
 * load.h - a WAV becomes a track.
 *
 * The one place the editor reaches down to a file. Kept apart from doc.c so the
 * model itself carries no file format with it and can be tested without one.
 *
 * The other direction is here too, for the two edits that make audio rather
 * than rearrange it - see repair.h and limit.h. A block those produce came from
 * nowhere a project file could refer back to, so it has to reach a disk before
 * it reaches the timeline, and both of them want the same WAV written the same
 * way.
 *
 * There is no resampling here, and that is deliberate rather than unfinished:
 * audiaki declines to play a stream at the wrong rate rather than play it at
 * the wrong pitch, and a project that quietly mixed 48 kHz audio into a 44.1
 * kHz session would be doing exactly that.
 */
#ifndef AUDIAKI_EDIT_LOAD_H
#define AUDIAKI_EDIT_LOAD_H

#include "edit/doc.h"
#include "edit/samples.h"

/*
 * Read the whole of `path` into a block, stamped with where it came from so a
 * project can refer to it later. `*out_rate` is the file's sample rate.
 *
 * Returns the block, which the caller owns one reference to, or NULL with
 * `*why` set. Shared with the project loader, which rebuilds clips over blocks
 * rather than making a track of each file.
 */
aud_samples *aud_edit_read_wav(const char *path, unsigned *out_rate, const char **why);

/*
 * Read `path` and append it to `d` as a new track named after the file.
 *
 * An empty project takes its sample rate from the first file loaded into it. A
 * file at a different rate from a project that already has one is refused.
 *
 * Returns the index of the new track, or -1 with `*why` set to a static
 * description of what went wrong. `why` may be NULL.
 */
int aud_edit_load_wav(aud_doc *d, const char *path, const char **why);

/*
 * Write `block` into `dir` as a 24-bit WAV, under the next free name beginning
 * `prefix`, and put where it landed in `path`.
 *
 * Numbered rather than overwritten, by the same aud_take_next() a take is
 * named through: an edit that made audio should never be the moment somebody
 * discovers it has replaced something.
 *
 * Twenty-four bits because that is what audiaki records at, and because the
 * block is going straight back onto a timeline beside takes at that width. A
 * sample past full scale is clamped on the way out - a float block is allowed
 * to hold one and a PCM file is not.
 *
 * Returns 0, or -1 with `*why` set to a static description. `why` may be NULL.
 */
int aud_edit_write_block(const aud_samples *block, unsigned rate, const char *dir,
                         const char *prefix, char *path, size_t size, const char **why);

#endif /* AUDIAKI_EDIT_LOAD_H */
