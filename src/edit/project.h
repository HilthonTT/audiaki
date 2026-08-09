/* SPDX-License-Identifier: MIT */
/*
 * project.h - a session, written down.
 *
 * Everything an edit does is expressed as clips pointing at windows of shared
 * blocks (see track.h), and that is exactly what makes a project cheap to save:
 * the audio is already on disk as the takes and imports it came from, so a
 * project file is a list of which parts of which files sit where. A session of
 * hundreds of megabytes writes out as a few kilobytes of text, and saving is
 * fast enough to do on every edit if it ever needs to be.
 *
 * The format is line-based text, for the same reasons config.h's is: it can be
 * read, diffed, fixed by hand and put in version control, none of which is true
 * of a packed binary that only this program understands.
 *
 *   audiaki-project 1
 *   rate 48000
 *   cursor 0
 *   selection 0 0
 *   source takes/take-001.wav
 *   track
 *   name Take 1
 *   channels 2
 *   gain 1.000000
 *   clip 0 0 480000 0 0 2048
 *
 * `source` lines are numbered by the order they appear in, and a clip names one
 * by index: `clip SOURCE OFFSET FRAMES START FADE_IN FADE_OUT`, all in frames.
 * Sources under the project's own folder are written relative to it, so a
 * session folder can be copied to another disk and still open.
 *
 * What is deliberately not here is the audio. A project refers to the takes; it
 * does not contain them. Move a take away and the project says which one is
 * missing rather than opening with a silent lane - losing audio quietly is the
 * one failure a recorder must not have.
 *
 * Reads and writes through media/wav.h and util/path.h, and touches no sound
 * server, so it builds and is tested anywhere - see the layout rule in
 * DESIGN.md.
 */
#ifndef AUDIAKI_EDIT_PROJECT_H
#define AUDIAKI_EDIT_PROJECT_H

#include "edit/doc.h"

/* What a project file is called, and the version this reads and writes. */
#define AUD_PROJECT_EXT ".aki"
#define AUD_PROJECT_MAGIC "audiaki-project"
#define AUD_PROJECT_VERSION 1

/*
 * Distinct audio files one project may refer to. Comfortably more takes than a
 * session has, and small enough that a corrupt file is refused rather than
 * turned into an enormous allocation.
 */
#define AUD_PROJECT_MAX_SOURCES 512u

/*
 * Write `d` to `path`, replacing whatever was there.
 *
 * Returns 0, or -1 with `*why` set to a description of what went wrong. `why`
 * may be NULL. A block that has no source file - one that came from nowhere a
 * reader could go back to - is the one thing that cannot be written, and it is
 * named rather than silently dropped.
 *
 * The file is written beside its destination and renamed into place, so an
 * interrupted save leaves the previous version of the project intact.
 */
int aud_project_save(const aud_doc *d, const char *path, const char **why);

/*
 * Read `path` into `d`, replacing everything it held.
 *
 * All or nothing: the project is built up separately and only swapped in once
 * every source has been found and read, so a load that fails leaves whatever
 * was open alone. Returns 0, or -1 with `*why` set.
 */
int aud_project_load(aud_doc *d, const char *path, const char **why);

/* Non-zero when `path` ends in the project extension, whatever its case. */
int aud_project_is_project(const char *path);

#endif /* AUDIAKI_EDIT_PROJECT_H */
