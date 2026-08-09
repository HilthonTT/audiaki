/* SPDX-License-Identifier: MIT */
/*
 * path.h - the filesystem side of deciding where a take ends up.
 *
 * Both halves of audiaki ask the same two questions once a take is over -
 * which folder, and under what name - and neither of them may lose the
 * recording while answering. That is all this is: expanding what someone
 * typed, joining a folder to a name, creating the folder, and moving a file
 * into it without ever landing on something that is already there.
 *
 * No audio and no sound server, so it is unit tested like the rest of util/.
 */
#ifndef AUDIAKI_PATH_H
#define AUDIAKI_PATH_H

#include <stddef.h>

/*
 * Longer than any path Linux will accept, so a name is rejected by the kernel
 * for being too long rather than quietly truncated here into a different one.
 */
#define AUD_PATH_MAX 4096u

/*
 * Copy `path` into `dst` with a leading '~' replaced by $HOME. Only a leading
 * one, and only when it stands alone or is followed by a slash: "~/takes" is a
 * home directory, "~user/takes" is another user's and this does not know how to
 * find it, and "a~b.wav" is a filename.
 *
 * Returns 0 on success, or -1 with dst untouched when the result would not fit
 * or there is no $HOME to expand against.
 */
int aud_path_expand(char *dst, size_t size, const char *path);

/*
 * The other way round: copy `path` into `dst` with $HOME written back as '~'.
 *
 * For showing a path to someone rather than for opening one. A home directory
 * spelled out in full is most of the width of a prompt and none of the
 * information in it, and what comes back through aud_path_expand() is the same
 * path again.
 *
 * Returns 0 on success, or -1 with dst untouched when it would not fit.
 */
int aud_path_shorten(char *dst, size_t size, const char *path);

/*
 * Join `dir` and `name` with a single separator between them.
 *
 * An empty or NULL `dir` gives `name` alone, and so does a `name` that is
 * already absolute: naming the file outright is a stronger statement than a
 * configured default folder, and the folder does not get to move it.
 *
 * Returns 0 on success, -1 with dst untouched when it would not fit.
 */
int aud_path_join(char *dst, size_t size, const char *dir, const char *name);

/*
 * Put `name` in `dir`, unless `name` has already said where it goes.
 *
 * A name with a slash in it is a place: 'takes/riff.wav' means that folder,
 * relative to wherever the caller is, and a configured default folder has no
 * business moving it somewhere else. A bare name is only a name, and that is
 * exactly what a default folder is for.
 *
 * Returns 0 on success, -1 with dst untouched when it would not fit.
 */
int aud_path_place(char *dst, size_t size, const char *dir, const char *name);

/* The part of `path` after its last slash, which is never NULL. */
const char *aud_path_basename(const char *path);

/*
 * The part of `path` before its last slash, written to `dst`. A path with no
 * slash in it gives "." - the working directory is where it would be created.
 *
 * Returns 0 on success, -1 with dst untouched when it would not fit.
 */
int aud_path_dirname(char *dst, size_t size, const char *path);

/* Non-zero when `path` exists and is a directory. */
int aud_path_is_dir(const char *path);

/*
 * Create `dir` and any of its parents that are missing, like 'mkdir -p'.
 * A directory that already exists is a success rather than an EEXIST.
 *
 * Returns 0 on success, -1 with errno set by whichever component failed.
 */
int aud_path_mkdirs(const char *dir);

/*
 * Move `src` to `dst`, which must not already exist.
 *
 * Deliberately not rename(2): that replaces its destination without a word,
 * and the one thing this must never do is silently drop a take on top of an
 * older one. A hard link claims the name atomically instead, and only once it
 * is held does the original go away. Across filesystems - a take being moved
 * onto a memory stick - there is no link to make, so the bytes are copied under
 * an exclusive create and the original is removed after they all arrived.
 *
 * Returns 0 on success, or -1 with errno set: EEXIST means `dst` was taken and
 * `src` is exactly where it was.
 */
int aud_path_move(const char *src, const char *dst);

#endif /* AUDIAKI_PATH_H */
