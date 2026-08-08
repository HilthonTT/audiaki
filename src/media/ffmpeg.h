/* SPDX-License-Identifier: MIT */
/*
 * ffmpeg.h - render raw frames into a video file through an ffmpeg child.
 *
 * ffmpeg is not linked in; it is spawned, handed rawvideo on stdin and told to
 * mux it with an audio file it opens itself. That keeps the h264 and AAC
 * licensing, and the codec dependency list, outside this program.
 *
 * ffmpeg(1) must be on PATH at run time. It is not needed to build or to
 * record - only to render.
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
 * pipe broke, so the only useful response is ffmpeg_end_rendering(f, 1).
 */
int ffmpeg_send_frame(FFMPEG *ffmpeg, const void *data, size_t width, size_t height);

/*
 * Same, but reading the rows bottom-up, for callers whose framebuffer has its
 * origin at the bottom left (OpenGL and most GPU readbacks).
 */
int ffmpeg_send_frame_flipped(FFMPEG *ffmpeg, const void *data, size_t width,
                              size_t height);

/*
 * Close the pipe and reap the child, freeing `ffmpeg` either way. With
 * `cancel` set, ffmpeg is killed instead of being allowed to finish, and a
 * non-zero exit is not reported as an error.
 *
 * Returns 0 when ffmpeg finished successfully, -1 otherwise. A cancelled
 * render returns 0: the caller asked for it.
 */
int ffmpeg_end_rendering(FFMPEG *ffmpeg, int cancel);

#endif /* AUDIAKI_FFMPEG_H */
