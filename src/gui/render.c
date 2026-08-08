/* SPDX-License-Identifier: MIT */
#include "gui/render.h"

#include "media/ffmpeg.h"
#include "media/wav.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* frames of audio decoded per read; unrelated to the video frame rate */
#define RENDER_CHUNK_FRAMES 4096u

struct aud_render
{
  wav_reader reader;
  int reader_open;

  aud_viz *viz;
  FFMPEG *ffmpeg;
  RenderTexture2D target;
  int target_ready;

  float *chunk;
  char output[AUD_RENDER_PATH_MAX];
  /* what ffmpeg actually writes, renamed onto `output` when it is complete */
  char partial[AUD_RENDER_PATH_MAX + 32];

  unsigned width;
  unsigned height;
  unsigned fps;

  uint64_t total_frames;
  uint64_t frame;
  uint64_t consumed; /* audio frames pushed so far */

  int failed;
};

void aud_render_defaults(aud_render_options *opts)
{
  if (opts == NULL)
  {
    return;
  }

  memset(opts, 0, sizeof(*opts));
  opts->mode = AUD_VIZ_MODE_BARS;
  opts->width = AUD_RENDER_DEFAULT_WIDTH;
  opts->height = AUD_RENDER_DEFAULT_HEIGHT;
  opts->fps = AUD_RENDER_DEFAULT_FPS;
  opts->bands = AUD_VIZ_DEFAULT_BANDS;
}

/*
 * Where the frames actually go while they are being written:
 * "takes/session-003.mp4" -> "takes/.session-003.partial.mp4".
 *
 * An MP4 is not playable until its moov atom is written, which ffmpeg only
 * does when the stream ends. Writing straight to the final name would put a
 * file on disk the instant recording stopped that looks like a finished take
 * and opens as "no playable streams" for as long as the render runs - and
 * leave that same broken file behind for good if the render never finished.
 *
 * So the final name is only ever created by rename(2), once the video is
 * complete. The extension is kept because ffmpeg picks its muxer from it, and
 * the leading dot keeps the partial file out of a file browser's way.
 */
static int partial_path(char *dst, size_t size, const char *final)
{
  const char *slash = strrchr(final, '/');
  size_t dir_len = slash != NULL ? (size_t)(slash - final) + 1u : 0u;
  const char *base = final + dir_len;
  const char *dot = strrchr(base, '.');
  size_t stem_len;
  const char *ext;
  int written;

  /* a leading dot is a hidden file, not an extension */
  if (dot == NULL || dot == base)
  {
    stem_len = strlen(base);
    ext = "";
  }
  else
  {
    stem_len = (size_t)(dot - base);
    ext = dot;
  }

  written = snprintf(NULL, 0, "%.*s.%.*s.partial%s", (int)dir_len, final, (int)stem_len,
                     base, ext);
  if (written < 0 || (size_t)written >= size)
  {
    return -1;
  }

  snprintf(dst, size, "%.*s.%.*s.partial%s", (int)dir_len, final, (int)stem_len, base,
           ext);
  return 0;
}

aud_render *aud_render_start(const aud_render_options *opts)
{
  aud_render *r;

  if (opts == NULL || opts->wav_path == NULL || opts->video_path == NULL ||
      opts->width == 0 || opts->height == 0 || opts->fps == 0)
  {
    errno = EINVAL;
    return NULL;
  }

  r = calloc(1, sizeof(*r));
  if (r == NULL)
  {
    aud_error("out of memory");
    return NULL;
  }

  r->width = opts->width;
  r->height = opts->height;
  r->fps = opts->fps;

  if (snprintf(r->output, sizeof(r->output), "%s", opts->video_path) < 0 ||
      strlen(opts->video_path) >= sizeof(r->output) ||
      partial_path(r->partial, sizeof(r->partial), r->output) != 0)
  {
    aud_error("that video path is too long");
    free(r);
    return NULL;
  }

  if (wav_read_open(&r->reader, opts->wav_path) != 0)
  {
    aud_error("cannot read %s: %s", opts->wav_path,
              r->reader.error != NULL ? r->reader.error : "unrecognised file");
    free(r);
    return NULL;
  }
  r->reader_open = 1;

  if (r->reader.frames == 0)
  {
    aud_warn("%s is empty, so there is nothing to render", opts->wav_path);
    goto fail;
  }

  /* ceil, so the last partial video frame is still rendered */
  r->total_frames =
      (r->reader.frames * r->fps + r->reader.rate - 1u) / (uint64_t)r->reader.rate;
  if (r->total_frames == 0)
  {
    r->total_frames = 1;
  }

  r->chunk = malloc(RENDER_CHUNK_FRAMES * sizeof(*r->chunk));
  if (r->chunk == NULL)
  {
    aud_error("cannot set up the renderer");
    goto fail;
  }

  /* the analyser runs at the take's rate, which need not be the device's */
  r->viz = aud_viz_create(r->reader.rate, opts->bands);
  if (r->viz == NULL)
  {
    aud_error("cannot set up the renderer's visualiser");
    goto fail;
  }
  aud_viz_set_mode(r->viz, opts->mode);

  r->target = LoadRenderTexture((int)r->width, (int)r->height);
  if (r->target.id == 0)
  {
    aud_error("cannot allocate a %ux%u render target", r->width, r->height);
    goto fail;
  }
  r->target_ready = 1;

  r->ffmpeg = ffmpeg_start_rendering(r->partial, r->width, r->height, r->fps,
                                     opts->silent ? NULL : opts->wav_path);
  if (r->ffmpeg == NULL)
  {
    goto fail;
  } /* ffmpeg_start_rendering has already said why */

  aud_info("rendering %s: %ux%u at %u fps, %s%s", r->output, r->width, r->height, r->fps,
           aud_viz_mode_name(opts->mode), opts->silent ? ", no audio" : "");
  return r;

fail:
  aud_render_finish(r, 1);
  return NULL;
}

/* Feed the analyser everything up to the end of the current video frame. */
static int advance_audio(aud_render *r)
{
  uint64_t target = ((r->frame + 1u) * (uint64_t)r->reader.rate) / r->fps;
  size_t need = (size_t)(target - r->consumed);

  while (need > 0)
  {
    size_t take = need < RENDER_CHUNK_FRAMES ? need : RENDER_CHUNK_FRAMES;
    long got = wav_read_mono(&r->reader, r->chunk, take);

    if (got < 0)
    {
      aud_error("cannot read the take: %s",
                r->reader.error != NULL ? r->reader.error : "read error");
      return -1;
    }
    if (got == 0)
    {
      /* past the end of the audio: feed silence so the display falls away */
      memset(r->chunk, 0, take * sizeof(*r->chunk));
      got = (long)take;
    }

    aud_viz_push(r->viz, r->chunk, (size_t)got);
    need -= (size_t)got;
  }

  r->consumed = target;
  return 0;
}

static int emit_frame(aud_render *r)
{
  Rectangle area = {0.0f, 0.0f, (float)r->width, (float)r->height};
  Image shot;
  int rc;

  aud_viz_update(r->viz, 1.0f / (float)r->fps);

  BeginTextureMode(r->target);
  ClearBackground(BLACK);
  aud_viz_draw(r->viz, area);
  EndTextureMode();

  shot = LoadImageFromTexture(r->target.texture);
  if (shot.data == NULL)
  {
    aud_error("cannot read back the rendered frame");
    return -1;
  }

  /*
   * Flipped: a render texture has its origin at the bottom left, and ffmpeg
   * wants the top row first.
   */
  rc = ffmpeg_send_frame_flipped(r->ffmpeg, shot.data, r->width, r->height);
  UnloadImage(shot);

  if (rc != 0)
  {
    aud_error("the encoder stopped accepting frames");
    return -1;
  }
  return 0;
}

int aud_render_step(aud_render *r, double budget)
{
  double started;

  if (r == NULL || r->failed)
  {
    return -1;
  }
  if (r->frame >= r->total_frames)
  {
    return 1;
  }

  started = GetTime();

  do
  {
    if (advance_audio(r) != 0 || emit_frame(r) != 0)
    {
      r->failed = 1;
      return -1;
    }
    r->frame++;
  } while (r->frame < r->total_frames && GetTime() - started < budget);

  return r->frame >= r->total_frames ? 1 : 0;
}

double aud_render_progress(const aud_render *r)
{
  if (r == NULL || r->total_frames == 0)
  {
    return 0.0;
  }

  return (double)r->frame / (double)r->total_frames;
}

const char *aud_render_output(const aud_render *r)
{
  return r != NULL ? r->output : "";
}

int aud_render_finish(aud_render *r, int cancel)
{
  int rc = 0;

  if (r == NULL)
  {
    return -1;
  }

  if (r->ffmpeg != NULL)
  {
    /* the moov atom is written as the pipe closes, so this has to come first */
    rc = ffmpeg_end_rendering(r->ffmpeg, cancel);

    if (cancel || rc != 0 || r->frame < r->total_frames)
    {
      /*
       * Killed, failed, or stopped short: whatever ffmpeg left is not a
       * playable video, and leaving it under the take's name would be worse
       * than leaving nothing at all.
       */
      remove(r->partial);
      rc = cancel ? 0 : -1;
    }
    else if (rename(r->partial, r->output) != 0)
    {
      aud_perror("cannot move the finished video to %s", r->output);
      remove(r->partial);
      rc = -1;
    }
  }
  else
  {
    /* nothing was ever started, so there is nothing to salvage or clean up */
    rc = -1;
  }

  if (r->target_ready)
  {
    UnloadRenderTexture(r->target);
  }
  aud_viz_destroy(r->viz);
  if (r->reader_open)
  {
    wav_read_close(&r->reader);
  }
  free(r->chunk);
  free(r);

  return rc;
}
