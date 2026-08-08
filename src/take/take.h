/* SPDX-License-Identifier: MIT */
/*
 * take.h - numbered take filenames.
 *
 * Tracking a part means recording it several times, and naming each attempt by
 * hand is exactly the kind of friction that makes people reach for --force and
 * lose a good take. --take hands out the next free number instead.
 *
 * No ALSA and no audio, so it can be unit tested anywhere.
 */
#ifndef AUDIAKI_TAKE_H
#define AUDIAKI_TAKE_H

#include <stddef.h>

/*
 * Numbers are zero padded to three digits and then allowed to grow, so an
 * ordinary session sorts correctly in a file browser and a very long one still
 * gets a name. Past this it is a runaway loop rather than a session.
 */
#define AUD_TAKE_MAX_NUMBER 9999u

/*
 * Build the path for take `number` of `prefix`.
 *
 * "session" and 3 give "session-003.wav". A prefix that already carries an
 * extension keeps it - "session.wav" also gives "session-003.wav" - because
 * typing the name of the file you expect is the obvious thing to do.
 *
 * Returns 0 on success, or -1 with dst untouched when the result would not fit
 * in `size` or the arguments are unusable.
 */
int aud_take_path(char *dst, size_t size, const char *prefix, unsigned number);

/*
 * Fill `dst` with the first take of `prefix` that does not already exist.
 *
 * Inherently racy against another process creating the same file, which is why
 * the recorder still opens the result without --force: this picks a name, and
 * wav_open() is what actually claims it.
 *
 * Returns 0 on success, -1 when the name would not fit or every number up to
 * AUD_TAKE_MAX_NUMBER is taken.
 */
int aud_take_next(char *dst, size_t size, const char *prefix);

/*
 * Write `path` into `dst` with its extension replaced by `ext`, which includes
 * the dot: "takes/session-003.wav" and ".mp4" give "takes/session-003.mp4".
 *
 * A dot inside a directory name is not an extension, so "a.b/take" becomes
 * "a.b/take.mp4" rather than "a.mp4". A path with no extension gains one.
 *
 * Returns 0 on success, or -1 with dst untouched when the result would not fit
 * in `size` or the arguments are unusable.
 */
int aud_take_with_extension(char *dst, size_t size, const char *path, const char *ext);

#endif /* AUDIAKI_TAKE_H */
