/* SPDX-License-Identifier: MIT */
/*
 * repair.h - the spectral edit, carried out on the project.
 *
 * audio/spectral.h knows how to measure a buffer and how to filter one, and
 * nothing about tracks. This is the other half: which frames of which lane to
 * measure, and what to do with what comes back.
 *
 * It is the one edit that is not clip surgery, and it is worth being clear
 * about why. Everything in edit.h is a window moving over audio that never
 * changes - see samples.h - which is what makes cutting an hour instant and
 * undo affordable. There is no arrangement of clips that means "this take,
 * without the hum", so this one has to produce audio that did not exist before:
 * the range is read out, filtered, and put back as a new block.
 *
 * Two things follow from that, and both are deliberate:
 *
 * It costs the range. A minute of stereo is about twenty megabytes and a second
 * or so of work, where a cut costs neither. That is the price of the only
 * operation here that changes what a sample reads as.
 *
 * It writes a WAV. A project file is a list of which parts of which files sit
 * where, so a block that came from nowhere a reader could go back to is the one
 * thing aud_project_save() cannot write down - see project.h. Rather than hand
 * the user a session that turns out not to be saveable an hour later, the new
 * audio goes to disk as it is made, and the block is stamped with where.
 *
 * An undo leaves that file behind. It is not deleted, and that is not an
 * oversight: the redo stack still refers to the block, and a file removed under
 * it would turn redo into silence. A repair that was undone leaves a WAV in the
 * takes folder that nothing points at, which is a file to delete rather than
 * audio to lose.
 */
#ifndef AUDIAKI_EDIT_REPAIR_H
#define AUDIAKI_EDIT_REPAIR_H

#include "audio/spectral.h"
#include "edit/doc.h"

#include <stdint.h>

/*
 * Windows a reading is made from, however long the range is. A few hundred
 * spread across a take says the same thing about it as every window in it
 * would, and says it while the pointer is still moving: the panel re-reads
 * whenever the selection changes, so this is a budget for one frame of
 * drawing rather than for a batch job.
 */
#define AUD_REPAIR_MAX_WINDOWS 400u

/* What the cleaned-up audio is called, before its number. */
#define AUD_REPAIR_PREFIX "cleaned"

/* The undo step a repair pushes, which is what the Undo button offers back. */
#define AUD_REPAIR_LABEL "clean up"

/*
 * Measure frames [from, to) of track `index` into `s`, as a reading.
 *
 * The windows are spread evenly across the range rather than taken from the
 * start of it, so a reading of a whole take is a reading of the whole take.
 * Channels are averaged: a hum is a hum on both sides, and one graph is easier
 * to read than two.
 *
 * Returns 0, or -1 when the arguments do not describe any audio, or there is no
 * memory for the buffers. A failure leaves whatever `s` held before.
 */
int aud_repair_read(const aud_doc *d, size_t index, uint64_t from, uint64_t to,
                    aud_spectral *s);

/*
 * Replace frames [from, to) of track `index` with the same audio put through
 * `s`, writing the result into `dir` as a WAV so a project can refer to it.
 *
 * Takes a checkpoint, so this is one press of Undo like any other edit. Every
 * step that can fail happens before anything is changed, so a -1 leaves the
 * project exactly as it was, checkpoint included.
 *
 * The run-up either side that aud_spectral_context() asks for is read from the
 * track and thrown away again, so the repaired stretch joins onto the audio
 * around it without a seam at either end.
 *
 * What comes back is one clip over one block, whatever the range was made of
 * before. Gaps inside it become real silence, which sounds the same and is what
 * "this stretch, filtered" has to mean.
 *
 * Returns 0, or -1 with `*why` set to a static description. `why` may be NULL.
 */
int aud_repair_apply(aud_doc *d, size_t index, uint64_t from, uint64_t to,
                     aud_spectral *s, const char *dir, const char **why);

#endif /* AUDIAKI_EDIT_REPAIR_H */
