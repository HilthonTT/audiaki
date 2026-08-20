/* SPDX-License-Identifier: MIT */
/*
 * limit.h - the limiter, carried out on the project.
 *
 * audio/limiter.h knows how to hold a buffer under a ceiling and nothing about
 * tracks. This is the other half: which frames of which lanes, where the audio
 * it produces is kept, and how it gets back onto the timeline.
 *
 * The reason it exists is one sentence in the window's own documentation.
 * Normalizing to a loudness target can push peaks past full scale - it is a
 * gated mean, and a take with one transient far above its average has to go
 * over to reach the target - and the answer until now was to do it and say so,
 * because a limiter nobody asked for would have been worse than an overshoot
 * that was reported. This is the limiter you ask for.
 *
 * Like repair.h, and unlike everything in edit.h, this is not clip surgery: it
 * produces audio that did not exist before, so it costs the range in memory and
 * in time, and it writes a WAV as it goes because a project file refers to files
 * rather than carrying samples - see project.h. Everything repair.h says about
 * that applies here word for word, including that an undo leaves the file
 * behind rather than deleting audio the redo stack still points at.
 *
 * What it is measured against is audio/truepeak.h, which is what --info reports
 * and what the window's peak normalize aims at. A lane put under -1 dBTP here
 * reads at or under -1 dBTP afterwards.
 */
#ifndef AUDIAKI_EDIT_LIMIT_H
#define AUDIAKI_EDIT_LIMIT_H

#include "edit/doc.h"

/* What the limited audio is called, before its number. */
#define AUD_LIMIT_PREFIX "limited"

/* The undo step this pushes, which is what the Undo button offers back. */
#define AUD_LIMIT_LABEL "limit"

/*
 * Hold the selection under `ceiling_db` dBTP, on every selected lane, writing
 * the audio it makes into `dir`.
 *
 * One lane at a time and one measurement each, the way aud_edit_normalize()
 * works and for the same reason: what makes two takes sit together is each of
 * them reaching the target, and a single gain worked out across every lane
 * would be a master fader wearing this name.
 *
 * A lane already under the ceiling is not touched at all - no audio is made for
 * it, no file is written, and its clips are left exactly as they were. That is
 * what makes this safe to reach for over a whole session: it is a no-op
 * everywhere it has nothing to do, and running it twice does nothing the second
 * time.
 *
 * What it does touch comes back as one clip over one block, whatever the range
 * was made of before, with the fades and clip gains that were there folded into
 * the audio - they have been applied on the way out and cannot be applied
 * twice. Gaps inside the range become real silence, which sounds the same and
 * is what "this stretch, limited" has to mean.
 *
 * `reduction_db` comes back as the most any lane was turned down by, and may be
 * NULL. Returns 0 when every lane that needed limiting got it, or -1 with
 * `*why` set to a static description - nothing selected, nothing over the
 * ceiling, or something went wrong on one of the lanes. `why` may be NULL.
 *
 * One checkpoint covers the whole press however many lanes it touched, so Undo
 * takes all of it back at once. A failure part way through - a disk that filled
 * between one lane and the next - therefore leaves the lanes it had already
 * done and says so, rather than reporting a success that quietly did less than
 * it was asked, and it is undone in a single step rather than in as many steps
 * as it happened to get through.
 */
int aud_limit_selection(aud_doc *d, double ceiling_db, const char *dir,
                        double *reduction_db, const char **why);

/*
 * The loudest true peak in the selection across the selected lanes, in dBTP.
 *
 * AUD_LUFS_NONE when there is nothing selected, or nothing but silence in what
 * is - the same "there is no figure here" audio/loudness.h uses, so a caller
 * needs no second path for it.
 *
 * This is what lets a normalize say what it has just done: a loudness target
 * reached by going 1.8 dB past full scale should be reported as that rather
 * than left for the export to discover.
 */
double aud_limit_peak_db(const aud_doc *d);

#endif /* AUDIAKI_EDIT_LIMIT_H */
