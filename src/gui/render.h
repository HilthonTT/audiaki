/* SPDX-License-Identifier: MIT */
/*
 * render.h - turn a finished take into a video of its visualiser.
 *
 * The video is rendered after the take is stopped, not captured live off the
 * screen. Recording is the job that must not miss a deadline: grabbing the
 * framebuffer and feeding an encoder sixty times a second, on the same machine
 * that is holding a capture stream open, is how takes end up with xruns in
 * them. Rendering afterwards costs the wait but is frame-accurate, is not tied
 * to the window's size or refresh rate, and cannot drop a frame.
 *
 * The frames are drawn by the same aud_viz the window uses, so the video shows
 * the visualiser that was on screen, in whichever style was selected - and
 * ffmpeg muxes in the take's own audio, so the two cannot drift.
 *
 * It runs in steps, a slice at a time from the main loop, because the drawing
 * needs the GL context that raylib keeps on that thread. A step takes a time
 * budget rather than a frame count, so the window stays responsive whether the
 * encoder is keeping up or not.
 */
#ifndef AUDIAKI_GUI_RENDER_H
#define AUDIAKI_GUI_RENDER_H

#include "viz.h"

#include <stddef.h>

#define AUD_RENDER_PATH_MAX 1024u

#define AUD_RENDER_DEFAULT_WIDTH 1280u
#define AUD_RENDER_DEFAULT_HEIGHT 720u
#define AUD_RENDER_DEFAULT_FPS 60u

/* Bounds for --video-size, matching what the CLI renderer accepts. */
#define AUD_RENDER_MIN_SIZE 160u
#define AUD_RENDER_MAX_SIZE 7680u

typedef struct
{
  const char *wav_path;   /* the finished take to read */
  const char *video_path; /* overwritten if it exists */
  aud_viz_mode mode;
  unsigned width;
  unsigned height;
  unsigned fps;
  size_t bands;
  /*
   * Leave the take's audio out of the video. The take is still read - it is
   * what the picture is drawn from - it just is not muxed in, for a video
   * headed somewhere the sound would be laid over separately or not wanted at
   * all. Off by default: a visualiser without its audio is the unusual want.
   */
  int silent;
} aud_render_options;

typedef struct aud_render aud_render;

void aud_render_defaults(aud_render_options *opts);

/*
 * Open the take, start ffmpeg and get ready to draw. Returns NULL after
 * reporting the reason through log.h - a missing ffmpeg or an unreadable take
 * is not fatal to the recording, which is already safely on disk.
 */
aud_render *aud_render_start(const aud_render_options *opts);

/*
 * Render for up to `budget` seconds. Returns 1 when the video is complete, 0
 * when there is more to do, and -1 on failure. Call aud_render_finish() in
 * every case.
 */
int aud_render_step(aud_render *r, double budget);

/* How far along, 0.0 to 1.0. */
double aud_render_progress(const aud_render *r);

/* The video being written, for the progress line. */
const char *aud_render_output(const aud_render *r);

/*
 * Close the encoder and free everything. With `cancel` set, ffmpeg is killed
 * and the partial video removed. Returns 0 when the video was written, -1
 * otherwise; a cancelled render returns 0, because the caller asked for it.
 */
int aud_render_finish(aud_render *r, int cancel);

#endif /* AUDIAKI_GUI_RENDER_H */
