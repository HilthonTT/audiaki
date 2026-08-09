/* SPDX-License-Identifier: MIT */
/*
 * load.h - a WAV becomes a track.
 *
 * The one place the editor reaches down to a file. Kept apart from doc.c so the
 * model itself carries no file format with it and can be tested without one.
 *
 * There is no resampling here, and that is deliberate rather than unfinished:
 * audiaki declines to play a stream at the wrong rate rather than play it at
 * the wrong pitch, and a project that quietly mixed 48 kHz audio into a 44.1
 * kHz session would be doing exactly that.
 */
#ifndef AUDIAKI_EDIT_LOAD_H
#define AUDIAKI_EDIT_LOAD_H

#include "edit/doc.h"

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

#endif /* AUDIAKI_EDIT_LOAD_H */
