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

/* how much of the canvas half-height a full scale sample uses, for the traces */
#define VIZ_TRACE_SCALE 0.80

/* the window the oscilloscope shows, as a fraction of a second */
#define VIZ_SCOPE_SECONDS 0.04

/* colours */
#define VIZ_BG_TOP AUD_RGBA(0x16, 0x19, 0x21, 0xFF)
#define VIZ_BG_BOTTOM AUD_RGBA(0x0B, 0x0C, 0x10, 0xFF)
#define VIZ_BASELINE_COLOR AUD_RGBA(0x2A, 0x2F, 0x3B, 0xFF)
#define VIZ_PLAYHEAD_COLOR AUD_RGBA(0xF2, 0xF4, 0xF8, 0xFF)
#define VIZ_HUE_LOW 190.0  /* cyan for the bass end */
#define VIZ_HUE_SPAN 150.0 /* through blue and violet to pink at the top end */

/* how far the unplayed part of a waveform is dimmed */
#define VIZ_UNPLAYED_SHADE 0.30

const char *aud_visualize_style_name(aud_viz_style style)
{
  switch (style)
  {
  case AUD_VIZ_STYLE_BARS:
    return "bars";
  case AUD_VIZ_STYLE_SCOPE:
    return "scope";
  case AUD_VIZ_STYLE_WAVEFORM:
    return "waveform";
  default:
    return "unknown";
  }
}

/* ASCII case folding only, as in format.c: option names are not locale text. */
static int same_name(const char *given, const char *known)
{
  while (*given != '\0' && *known != '\0')
  {
    char c = (*given >= 'A' && *given <= 'Z') ? (char)(*given - 'A' + 'a') : *given;

    if (c != *known)
    {
      return 0;
    }
    given++;
    known++;
  }
  return *given == '\0' && *known == '\0';
}

int aud_visualize_style_from_name(const char *name, aud_viz_style *out)
{
  static const aud_viz_style table[] = {
      AUD_VIZ_STYLE_BARS,
      AUD_VIZ_STYLE_SCOPE,
      AUD_VIZ_STYLE_WAVEFORM,
  };

  if (name == NULL || out == NULL)
  {
    return -1;
  }

  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
  {
    if (same_name(name, aud_visualize_style_name(table[i])))
    {
      *out = table[i];
      return 0;
    }
  }
  return -1;
}

/* The background every style shares, so a take looks the same whichever it is. */
static void draw_background(aud_canvas *c)
{
  aud_canvas_fill_gradient(c, 0, 0, (long)c->width, (long)c->height, VIZ_BG_TOP,
                           VIZ_BG_BOTTOM);
}

static void draw_bars(aud_canvas *c, const float *bands, const float *caps, size_t n)
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
  {
    span = 1;
  }

  /*
   * Shrink the reflection if the strip below the baseline cannot hold it, so a
   * tall bar's reflection fades out inside the frame instead of being cut off
   * flat by the bottom edge. Matters for an unusually short --size.
   */
  reflection = (double)(height - baseline - 1) / (double)span;
  if (reflection > VIZ_REFLECTION)
  {
    reflection = VIZ_REFLECTION;
  }
  if (reflection < 0.0)
  {
    reflection = 0.0;
  }

  draw_background(c);
  aud_canvas_fill_rect(c, 0, baseline, width, 1, VIZ_BASELINE_COLOR);

  slot = width / (long)n;
  if (slot < 1)
  {
    slot = 1;
  }
  gap = slot / 6;
  if (gap < 1)
  {
    gap = 1;
  }
  bar_w = slot - gap;
  if (bar_w < 1)
  {
    bar_w = 1;
  }

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
    {
      aud_canvas_fill_rect(c, x, baseline - cap_h - (long)VIZ_CAP_THICKNESS, bar_w,
                           (long)VIZ_CAP_THICKNESS, aud_canvas_hsv(hue, 0.35, 1.0, 0xFF));
    }
  }
}

/*
 * One column of a trace, drawn as the span between the lowest and highest
 * sample it covers. A column that would round away to nothing still gets a
 * pixel, so a silent passage reads as a flat line rather than a gap.
 */
static void draw_column(aud_canvas *c, long x, long centre, long half, double lo,
                        double hi, uint32_t color)
{
  long top = centre - (long)(hi * (double)half + 0.5);
  long bottom = centre - (long)(lo * (double)half + 0.5);
  long h = bottom - top + 1;

  aud_canvas_fill_rect(c, x, top, 1, h < 1 ? 1 : h, color);
}

/* The oscilloscope: the last few milliseconds, drawn left to right. */
static void draw_scope(aud_canvas *c, const float *window, size_t n)
{
  long width = (long)c->width;
  long height = (long)c->height;
  long centre = height / 2;
  long half = (long)(VIZ_TRACE_SCALE * (double)height / 2.0);

  draw_background(c);
  aud_canvas_fill_rect(c, 0, centre, width, 1, VIZ_BASELINE_COLOR);

  if (n == 0)
  {
    return;
  }

  for (long x = 0; x < width; x++)
  {
    size_t from = (size_t)((uint64_t)x * n / (uint64_t)width);
    size_t to = (size_t)((uint64_t)(x + 1) * n / (uint64_t)width);
    double lo = 0.0;
    double hi = 0.0;
    double level;

    if (to <= from)
    {
      to = from + 1;
    }
    if (to > n)
    {
      to = n;
    }

    for (size_t i = from; i < to; i++)
    {
      double v = (double)window[i];

      if (v < lo)
      {
        lo = v;
      }
      if (v > hi)
      {
        hi = v;
      }
    }

    /*
     * The x axis here is time, not frequency, so the bars' left-to-right hue
     * ramp would be a lie. Colour follows level instead: loud is bright and
     * shifted towards pink, quiet stays a dim cyan.
     */
    level = hi > -lo ? hi : -lo;
    draw_column(c, x, centre, half, lo, hi,
                aud_canvas_hsv(VIZ_HUE_LOW + VIZ_HUE_SPAN * level * 0.5, 0.65,
                               0.55 + 0.45 * level, 0xFF));
  }
}

/* The whole take at once, with the part already heard picked out. */
static void draw_waveform(aud_canvas *c, const float *lo, const float *hi, size_t columns,
                          double progress)
{
  long width = (long)c->width;
  long height = (long)c->height;
  long centre = height / 2;
  long half = (long)(VIZ_TRACE_SCALE * (double)height / 2.0);
  long playhead = (long)(progress * (double)width + 0.5);

  draw_background(c);
  aud_canvas_fill_rect(c, 0, centre, width, 1, VIZ_BASELINE_COLOR);

  for (long x = 0; x < width && (size_t)x < columns; x++)
  {
    double t = width > 1 ? (double)x / (double)(width - 1) : 0.0;
    uint32_t color = aud_canvas_hsv(VIZ_HUE_LOW + VIZ_HUE_SPAN * t, 0.70, 0.95, 0xFF);

    if (x >= playhead)
    {
      color = aud_canvas_shade(color, VIZ_UNPLAYED_SHADE);
    }
    draw_column(c, x, centre, half, (double)lo[x], (double)hi[x], color);
  }

  aud_canvas_fill_rect(c, playhead, 0, 2, height, VIZ_PLAYHEAD_COLOR);
}

/*
 * First pass for the waveform style: reduce the take to one min/max pair per
 * canvas column. The cost is bounded by the width of the video rather than the
 * length of the recording, so an hour costs the same few kilobytes as a minute.
 *
 * Columns no sample reaches keep their initial zeroes, which is what a
 * truncated file should look like: a flat line where the audio ran out.
 */
static int scan_envelope(const char *path, uint64_t frames, float *lo, float *hi,
                         size_t columns)
{
  wav_reader r;
  float *chunk = NULL;
  uint64_t index = 0;
  int rc = -1;

  if (frames == 0 || columns == 0)
  {
    return -1;
  }

  if (wav_read_open(&r, path) != 0)
  {
    aud_error("cannot re-read %s: %s", path,
              r.error != NULL ? r.error : "unrecognised file");
    return -1;
  }

  chunk = malloc(VIZ_CHUNK_FRAMES * sizeof(*chunk));
  if (chunk == NULL)
  {
    aud_perror("cannot scan %s", path);
    goto out;
  }

  for (;;)
  {
    long got = wav_read_mono(&r, chunk, VIZ_CHUNK_FRAMES);

    if (got < 0)
    {
      aud_error("cannot read %s: %s", path, r.error != NULL ? r.error : "read error");
      goto out;
    }
    if (got == 0)
    {
      break;
    }

    for (long f = 0; f < got; f++)
    {
      size_t col = (size_t)(index * (uint64_t)columns / frames);
      float v = chunk[f];

      if (col >= columns)
      {
        col = columns - 1;
      }
      if (v < lo[col])
      {
        lo[col] = v;
      }
      if (v > hi[col])
      {
        hi[col] = v;
      }
      index++;
    }
  }

  rc = 0;

out:
  free(chunk);
  wav_read_close(&r);
  return rc;
}

/*
 * Keep the newest `len` samples in `ring`, oldest first. The window is a few
 * milliseconds, so shuffling it beats the bookkeeping a real ring would need.
 */
static void scope_push(float *ring, size_t len, const float *src, size_t n)
{
  if (n >= len)
  {
    memcpy(ring, src + (n - len), len * sizeof(*ring));
    return;
  }
  memmove(ring, ring + n, (len - n) * sizeof(*ring));
  memcpy(ring + len - n, src, n * sizeof(*ring));
}

static void draw_progress(uint64_t frame, uint64_t total)
{
  if (aud_log_get_level() < AUD_LOG_NORMAL || !isatty(STDERR_FILENO))
  {
    return;
  }

  fprintf(stderr, "\r rendering %3.0f%%  frame %llu/%llu",
          100.0 * (double)frame / (double)total, (unsigned long long)frame,
          (unsigned long long)total);
  fflush(stderr);
}

static void clear_progress(void)
{
  if (aud_log_get_level() < AUD_LOG_NORMAL || !isatty(STDERR_FILENO))
  {
    return;
  }

  fprintf(stderr, "\r%50s\r", "");
  fflush(stderr);
}

void aud_visualize_defaults(aud_visualize_options *opts)
{
  if (opts == NULL)
  {
    return;
  }

  memset(opts, 0, sizeof(*opts));
  opts->width = AUD_VIZ_DEFAULT_WIDTH;
  opts->height = AUD_VIZ_DEFAULT_HEIGHT;
  opts->fps = AUD_VIZ_DEFAULT_FPS;
  opts->bars = AUD_VIZ_DEFAULT_BARS;
  opts->style = AUD_VIZ_STYLE_BARS;
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
  float *scope = NULL;
  float *env_lo = NULL;
  float *env_hi = NULL;
  size_t scope_frames = 0;
  uint64_t total_video_frames;
  uint64_t total_audio_frames;
  uint64_t consumed = 0;
  double dt;
  int needs_audio;
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
    {
      aud_error("cannot read %s: %s (%s)", opts->input_path, reader.error,
                strerror(errno));
    }
    else
    {
      aud_error("cannot read %s: %s", opts->input_path,
                reader.error != NULL ? reader.error : "unrecognised file");
    }
    return -1;
  }

  if (reader.frames == 0)
  {
    aud_error("%s contains no audio", opts->input_path);
    wav_read_close(&reader);
    return -1;
  }

  /* ceil, so the last partial video frame is still rendered */
  total_audio_frames = reader.frames;
  total_video_frames =
      (reader.frames * opts->fps + reader.rate - 1u) / (uint64_t)reader.rate;
  if (total_video_frames == 0)
  {
    total_video_frames = 1;
  }

  if (aud_canvas_init(&canvas, opts->width, opts->height) != 0)
  {
    aud_perror("cannot allocate a %ux%u canvas", opts->width, opts->height);
    wav_read_close(&reader);
    return -1;
  }

  /*
   * The waveform style knows the whole take before the first frame is drawn,
   * so the render pass only has to follow the playhead. The other two are
   * looking at the audio as it goes past and have to decode it.
   */
  needs_audio = opts->style != AUD_VIZ_STYLE_WAVEFORM;

  chunk = malloc(VIZ_CHUNK_FRAMES * sizeof(*chunk));
  if (chunk == NULL)
  {
    aud_perror("cannot set up the renderer");
    goto out;
  }

  switch (opts->style)
  {
  case AUD_VIZ_STYLE_BARS:
    aud_spectrum_config_defaults(&spec_cfg, reader.rate, opts->bars);
    spec = aud_spectrum_create(&spec_cfg);
    caps = calloc(opts->bars, sizeof(*caps));
    if (spec == NULL || caps == NULL)
    {
      aud_perror("cannot set up the analyser");
      goto out;
    }
    break;

  case AUD_VIZ_STYLE_SCOPE:
    scope_frames = (size_t)(VIZ_SCOPE_SECONDS * (double)reader.rate);
    if (scope_frames < 2u)
    {
      scope_frames = 2u;
    }
    scope = calloc(scope_frames, sizeof(*scope));
    if (scope == NULL)
    {
      aud_perror("cannot set up the scope");
      goto out;
    }
    break;

  case AUD_VIZ_STYLE_WAVEFORM:
  default:
    env_lo = calloc(opts->width, sizeof(*env_lo));
    env_hi = calloc(opts->width, sizeof(*env_hi));
    if (env_lo == NULL || env_hi == NULL)
    {
      aud_perror("cannot set up the waveform");
      goto out;
    }
    break;
  }

  aud_info("rendering %s -> %s: %ux%u at %u fps, %s, %.2f s", opts->input_path,
           opts->output_path, opts->width, opts->height, opts->fps,
           aud_visualize_style_name(opts->style), wav_read_duration(&reader));
  aud_debug("input: %u Hz, %u ch, %u bit%s", reader.rate, reader.channels, reader.bits,
            reader.is_float ? " float" : "");

  if (env_lo != NULL && scan_envelope(opts->input_path, total_audio_frames, env_lo,
                                      env_hi, opts->width) != 0)
  {
    goto out;
  }

  ffmpeg = ffmpeg_start_rendering(opts->output_path, opts->width, opts->height, opts->fps,
                                  opts->input_path);
  if (ffmpeg == NULL)
  {
    goto out;
  }

  dt = 1.0 / (double)opts->fps;

  for (uint64_t frame = 0; frame < total_video_frames; frame++)
  {
    /*
     * Derive the sample position from the frame index rather than accumulating
     * a per-frame step, so 44100 Hz at 60 fps (735 samples) and 48000 at 30
     * (1600) both stay exactly in step with the audio ffmpeg is muxing.
     */
    uint64_t target = ((frame + 1u) * (uint64_t)reader.rate) / opts->fps;
    size_t need = needs_audio ? (size_t)(target - consumed) : 0;

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
        /* past the end of the audio: feed silence so the display falls away */
        memset(chunk, 0, take * sizeof(*chunk));
        got = (long)take;
      }

      if (spec != NULL)
      {
        aud_spectrum_push(spec, chunk, (size_t)got);
      }
      if (scope != NULL)
      {
        scope_push(scope, scope_frames, chunk, (size_t)got);
      }
      need -= (size_t)got;
    }
    consumed = target;

    if (spec != NULL)
    {
      const float *bands = aud_spectrum_analyse(spec, dt);

      for (size_t b = 0; b < opts->bars; b++)
      {
        float fallen = caps[b] - (float)(VIZ_CAP_FALL * dt);
        if (fallen < 0.0f)
        {
          fallen = 0.0f;
        }
        caps[b] = bands[b] > fallen ? bands[b] : fallen;
      }

      draw_bars(&canvas, bands, caps, opts->bars);
    }
    else if (scope != NULL)
    {
      draw_scope(&canvas, scope, scope_frames);
    }
    else
    {
      draw_waveform(&canvas, env_lo, env_hi, opts->width,
                    (double)consumed / (double)total_audio_frames);
    }

    if (ffmpeg_send_frame(ffmpeg, canvas.pixels, canvas.width, canvas.height) != 0)
    {
      clear_progress();
      goto out;
    }

    if (frame % opts->fps == 0)
    {
      draw_progress(frame, total_video_frames);
    }
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

  free(env_hi);
  free(env_lo);
  free(scope);
  free(caps);
  free(chunk);
  aud_spectrum_destroy(spec);
  aud_canvas_free(&canvas);
  wav_read_close(&reader);
  return rc;
}
