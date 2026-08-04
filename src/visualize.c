/* SPDX-License-Identifier: MIT */
#include "visualize.h"

#include "canvas.h"
#include "ffmpeg.h"
#include "log.h"
#include "signals.h"
#include "spectrum.h"
#include "wav.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* frames of audio decoded per read; unrelated to the video frame rate */
#define VIZ_CHUNK_FRAMES 4096u

/* layout, as fractions of the canvas height */
#define VIZ_BASELINE 0.74    /* where the bars stand */
#define VIZ_TOP_MARGIN 0.08  /* headroom above a full scale bar */
#define VIZ_REFLECTION 0.35  /* reflected bar height, relative to the bar */
#define VIZ_CAP_FALL 0.80    /* peak marker fall, in bar heights per second */
#define VIZ_CAP_THICKNESS 3u /* peak marker height, in pixels */

/* colours */
#define VIZ_BG_TOP AUD_RGBA(0x16, 0x19, 0x21, 0xFF)
#define VIZ_BG_BOTTOM AUD_RGBA(0x0B, 0x0C, 0x10, 0xFF)
#define VIZ_BASELINE_COLOR AUD_RGBA(0x2A, 0x2F, 0x3B, 0xFF)
#define VIZ_HUE_LOW 190.0  /* cyan for the bass end */
#define VIZ_HUE_SPAN 150.0 /* through blue and violet to pink at the top end */

static void draw_frame(aud_canvas *c, const float *bands, const float *caps, size_t n)
{
  long height = (long)c->height;
  long width = (long)c->width;
  long baseline = (long)(VIZ_BASELINE * (double)height);
  long span = baseline - (long)(VIZ_TOP_MARGIN * (double)height);
  long slot;
  long gap;
  long bar_w;
  double reflection;

  if (span < 1)
    span = 1;

  /*
   * Shrink the reflection if the strip below the baseline cannot hold it, so a
   * tall bar's reflection fades out inside the frame instead of being cut off
   * flat by the bottom edge. Matters for an unusually short --size.
   */
  reflection = (double)(height - baseline - 1) / (double)span;
  if (reflection > VIZ_REFLECTION)
    reflection = VIZ_REFLECTION;
  if (reflection < 0.0)
    reflection = 0.0;

  aud_canvas_fill_gradient(c, 0, 0, width, height, VIZ_BG_TOP, VIZ_BG_BOTTOM);
  aud_canvas_fill_rect(c, 0, baseline, width, 1, VIZ_BASELINE_COLOR);

  slot = width / (long)n;
  if (slot < 1)
    slot = 1;
  gap = slot / 6;
  if (gap < 1)
    gap = 1;
  bar_w = slot - gap;
  if (bar_w < 1)
    bar_w = 1;

  for (size_t b = 0; b < n; b++)
  {
    double level = (double)bands[b];
    double t = n > 1 ? (double)b / (double)(n - 1) : 0.0;
    double hue = VIZ_HUE_LOW + VIZ_HUE_SPAN * t;
    long x = (long)b * slot + gap / 2;
    long bar_h = (long)(level * (double)span + 0.5);
    long cap_h = (long)((double)caps[b] * (double)span + 0.5);
    uint32_t tip;
    uint32_t root;

    /* a loud bar is brighter as well as taller, so peaks read at a glance */
    tip = aud_canvas_hsv(hue, 0.70, 0.65 + 0.35 * level, 0xFF);
    root = aud_canvas_hsv(hue, 0.90, 0.30 + 0.25 * level, 0xFF);

    if (bar_h > 0)
    {
      aud_canvas_fill_gradient(c, x, baseline - bar_h, bar_w, bar_h, tip, root);

      /* the reflection below the baseline; dim, and shorter than the bar */
      aud_canvas_fill_gradient(
          c, x, baseline + 1, bar_w, (long)((double)bar_h * reflection),
          aud_canvas_shade(root, 0.45), aud_canvas_shade(root, 0.05));
    }

    /* peak marker: hangs at the recent maximum and sinks back down */
    if (cap_h > bar_h)
      aud_canvas_fill_rect(c, x, baseline - cap_h - (long)VIZ_CAP_THICKNESS, bar_w,
                           (long)VIZ_CAP_THICKNESS, aud_canvas_hsv(hue, 0.35, 1.0, 0xFF));
  }
}

static void draw_progress(uint64_t frame, uint64_t total)
{
  if (aud_log_get_level() < AUD_LOG_NORMAL || !isatty(STDERR_FILENO))
    return;

  fprintf(stderr, "\r rendering %3.0f%%  frame %llu/%llu",
          100.0 * (double)frame / (double)total, (unsigned long long)frame,
          (unsigned long long)total);
  fflush(stderr);
}

static void clear_progress(void)
{
  if (aud_log_get_level() < AUD_LOG_NORMAL || !isatty(STDERR_FILENO))
    return;

  fprintf(stderr, "\r%50s\r", "");
  fflush(stderr);
}

void aud_visualize_defaults(aud_visualize_options *opts)
{
  if (opts == NULL)
    return;

  memset(opts, 0, sizeof(*opts));
  opts->width = AUD_VIZ_DEFAULT_WIDTH;
  opts->height = AUD_VIZ_DEFAULT_HEIGHT;
  opts->fps = AUD_VIZ_DEFAULT_FPS;
  opts->bars = AUD_VIZ_DEFAULT_BARS;
}

int aud_visualize_render(const aud_visualize_options *opts)
{
  wav_reader reader;
  aud_canvas canvas;
  aud_spectrum_config spec_cfg;
  aud_spectrum *spec = NULL;
  FFMPEG *ffmpeg = NULL;
  float *chunk = NULL;
  float *caps = NULL;
  uint64_t total_video_frames;
  uint64_t consumed = 0;
  double dt;
  int cancelled = 0;
  int rc = -1;

  if (opts == NULL || opts->input_path == NULL || opts->output_path == NULL)
  {
    aud_error("visualize: nothing to render");
    return -1;
  }

  if (wav_read_open(&reader, opts->input_path) != 0)
  {
    if (reader.error != NULL && errno != 0)
      aud_error("cannot read %s: %s (%s)", opts->input_path, reader.error,
                strerror(errno));
    else
      aud_error("cannot read %s: %s", opts->input_path,
                reader.error != NULL ? reader.error : "unrecognised file");
    return -1;
  }

  if (reader.frames == 0)
  {
    aud_error("%s contains no audio", opts->input_path);
    wav_read_close(&reader);
    return -1;
  }

  /* ceil, so the last partial video frame is still rendered */
  total_video_frames =
      (reader.frames * opts->fps + reader.rate - 1u) / (uint64_t)reader.rate;
  if (total_video_frames == 0)
    total_video_frames = 1;

  if (aud_canvas_init(&canvas, opts->width, opts->height) != 0)
  {
    aud_perror("cannot allocate a %ux%u canvas", opts->width, opts->height);
    wav_read_close(&reader);
    return -1;
  }

  aud_spectrum_config_defaults(&spec_cfg, reader.rate, opts->bars);
  spec = aud_spectrum_create(&spec_cfg);
  chunk = malloc(VIZ_CHUNK_FRAMES * sizeof(*chunk));
  caps = calloc(opts->bars, sizeof(*caps));

  if (spec == NULL || chunk == NULL || caps == NULL)
  {
    aud_perror("cannot set up the analyser");
    goto out;
  }

  aud_info("rendering %s -> %s: %ux%u at %u fps, %u bars, %.2f s", opts->input_path,
           opts->output_path, opts->width, opts->height, opts->fps, opts->bars,
           wav_read_duration(&reader));
  aud_debug("input: %u Hz, %u ch, %u bit%s", reader.rate, reader.channels, reader.bits,
            reader.is_float ? " float" : "");

  ffmpeg = ffmpeg_start_rendering(opts->output_path, opts->width, opts->height, opts->fps,
                                  opts->input_path);
  if (ffmpeg == NULL)
    goto out;

  dt = 1.0 / (double)opts->fps;

  for (uint64_t frame = 0; frame < total_video_frames; frame++)
  {
    /*
     * Derive the sample position from the frame index rather than accumulating
     * a per-frame step, so 44100 Hz at 60 fps (735 samples) and 48000 at 30
     * (1600) both stay exactly in step with the audio ffmpeg is muxing.
     */
    uint64_t target = ((frame + 1u) * (uint64_t)reader.rate) / opts->fps;
    size_t need = (size_t)(target - consumed);
    const float *bands;

    if (aud_signals_stop_requested())
    {
      cancelled = 1;
      break;
    }

    while (need > 0)
    {
      size_t take = need < VIZ_CHUNK_FRAMES ? need : VIZ_CHUNK_FRAMES;
      long got = wav_read_mono(&reader, chunk, take);

      if (got < 0)
      {
        clear_progress();
        aud_error("cannot read %s: %s", opts->input_path,
                  reader.error != NULL ? reader.error : "read error");
        goto out;
      }
      if (got == 0)
      {
        /* past the end of the audio: feed silence so the bars fall away */
        memset(chunk, 0, take * sizeof(*chunk));
        aud_spectrum_push(spec, chunk, take);
        need -= take;
        continue;
      }

      aud_spectrum_push(spec, chunk, (size_t)got);
      need -= (size_t)got;
    }
    consumed = target;

    bands = aud_spectrum_analyse(spec, dt);

    for (size_t b = 0; b < opts->bars; b++)
    {
      float fallen = caps[b] - (float)(VIZ_CAP_FALL * dt);
      if (fallen < 0.0f)
        fallen = 0.0f;
      caps[b] = bands[b] > fallen ? bands[b] : fallen;
    }

    draw_frame(&canvas, bands, caps, opts->bars);

    if (ffmpeg_send_frame(ffmpeg, canvas.pixels, canvas.width, canvas.height) != 0)
    {
      clear_progress();
      goto out;
    }

    if (frame % opts->fps == 0)
      draw_progress(frame, total_video_frames);
  }

  clear_progress();

  rc = ffmpeg_end_rendering(ffmpeg, cancelled);
  ffmpeg = NULL;

  if (cancelled)
  {
    aud_warn("cancelled, removing %s", opts->output_path);
    remove(opts->output_path);
  }
  else if (rc == 0)
  {
    aud_info("wrote %s", opts->output_path);
  }

out:
  if (ffmpeg != NULL)
  {
    /* an error on our side: stop ffmpeg and do not leave half a video behind */
    ffmpeg_end_rendering(ffmpeg, 1);
    remove(opts->output_path);
  }

  free(caps);
  free(chunk);
  aud_spectrum_destroy(spec);
  aud_canvas_free(&canvas);
  wav_read_close(&reader);
  return rc;
}
