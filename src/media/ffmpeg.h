/* SPDX-License-Identifier: MIT */
/*
 * ffmpeg.h - hand raw frames or raw samples to an ffmpeg child.
 *
 * ffmpeg is not linked in; it is spawned, handed a raw stream on stdin and
 * told what to make of it. That keeps the h264, AAC and MP3 licensing, and the
 * codec dependency list, outside this program.
 *
 * Two jobs, and they are the same shape twice: a video of the visualiser, and
 * a mixdown in a format audiaki does not write itself. Neither is needed to
 * build or to record - ffmpeg(1) has to be on PATH only when one of them is
 * actually asked for, and both say so plainly when it is not.
 *
 * The implementation is POSIX (fork/exec/pipe), hence ffmpeg_posix.c.
 */
#ifndef AUDIAKI_FFMPEG_H
#define AUDIAKI_FFMPEG_H

#include <stddef.h>

typedef struct FFMPEG FFMPEG;

/*
 * Start ffmpeg encoding `width` x `height` rawvideo at `fps`, muxed with the
 * audio in `sound_file_path`, into `output_path` - overwriting it if it exists,
 * so callers own the "refuse without --force" decision.
 *
 * A NULL `sound_file_path` writes a video with no audio track at all, for a
 * caller that wants the picture on its own.
 *
 * Returns NULL after reporting the reason through log.h. SIGPIPE is set to
 * SIG_IGN on success: if ffmpeg dies early, a send should fail with EPIPE
 * rather than take this process down with it.
 */
FFMPEG *ffmpeg_start_rendering(const char *output_path, size_t width, size_t height,
                               size_t fps, const char *sound_file_path);

/*
 * Write one frame of width * height RGBA pixels. `data` is a uint32_t array in
 * canvas.h's layout, read top row first.
 *
 * Returns 0 on success, -1 on failure. A failure means ffmpeg is gone or the
 * pipe broke, so the only useful response is ffmpeg_finish(f, 1).
 */
int ffmpeg_send_frame(FFMPEG *ffmpeg, const void *data, size_t width, size_t height);

/*
 * Same, but reading the rows bottom-up, for callers whose framebuffer has its
 * origin at the bottom left (OpenGL and most GPU readbacks).
 */
int ffmpeg_send_frame_flipped(FFMPEG *ffmpeg, const void *data, size_t width,
                              size_t height);

/* -- audio ------------------------------------------------------------------ */

/*
 * Start ffmpeg encoding interleaved little-endian PCM, arriving on stdin, into
 * `output_path` - overwriting it if it exists, so callers own the "refuse
 * without --force" decision as they do for a video.
 *
 * `bits` is 16, 24 or 32, and describes what will be sent rather than what
 * comes out: the container and codec are ffmpeg's to pick from the extension,
 * which is the same rule the video path follows. A lossless target keeps the
 * depth it is given; a lossy one has no use for it and converts.
 *
 * Returns NULL after reporting the reason through log.h.
 */
FFMPEG *ffmpeg_start_encoding(const char *output_path, unsigned rate, unsigned channels,
                              unsigned bits);

/*
 * Write `bytes` of that PCM. Returns 0 on success, -1 on failure, and a
 * failure means the same thing it does for a frame: ffmpeg is gone, and the
 * only useful response is ffmpeg_finish(f, 1).
 */
int ffmpeg_send_audio(FFMPEG *ffmpeg, const void *data, size_t bytes);

/* -- finishing either of them ----------------------------------------------- */

/*
 * Close the pipe and reap the child, freeing `ffmpeg` either way. With
 * `cancel` set, ffmpeg is killed instead of being allowed to finish, and a
 * non-zero exit is not reported as an error.
 *
 * Returns 0 when ffmpeg finished successfully, -1 otherwise. A cancelled job
 * returns 0: the caller asked for it.
 */
int ffmpeg_finish(FFMPEG *ffmpeg, int cancel);

#endif /* AUDIAKI_FFMPEG_H */
