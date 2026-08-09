/* SPDX-License-Identifier: MIT */
#include "gui/preview.h"

#include "backend/monitor.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

void aud_preview_init(aud_preview *p)
{
  if (p != NULL)
  {
    memset(p, 0, sizeof(*p));
  }
}

void aud_preview_stop(aud_preview *p)
{
  if (p == NULL)
  {
    return;
  }

  if (p->out != NULL)
  {
    aud_monitor_close(p->out);
    p->out = NULL;
  }
  if (p->open)
  {
    wav_read_close(&p->wav);
    p->open = 0;
  }

  free(p->buf);
  p->buf = NULL;
  p->playing = 0;
  p->written = 0;
  p->path[0] = '\0';
}

int aud_preview_start(aud_preview *p, const char *path, const char *device)
{
  aud_monitor_config cfg;

  if (p == NULL || path == NULL || path[0] == '\0')
  {
    return -1;
  }

  aud_preview_stop(p);

  if (wav_read_open(&p->wav, path) != 0)
  {
    aud_warn("cannot play %s: %s", path,
             p->wav.error != NULL ? p->wav.error : "unreadable");
    return -1;
  }
  p->open = 1;

  if (p->wav.frames == 0 || p->wav.channels == 0 || p->wav.rate == 0)
  {
    aud_warn("%s holds no audio to play", path);
    aud_preview_stop(p);
    return -1;
  }

  /*
   * At the file's own rate and channel count rather than the project's. This
   * is an audition of a file, and one played back at the wrong pitch because
   * it did not match the session would be worse than useless - it would be
   * misleading about the take.
   */
  aud_monitor_config_defaults(&cfg, p->wav.rate, p->wav.channels);
  cfg.name = device;

  p->out = aud_monitor_open(&cfg);
  if (p->out == NULL)
  {
    /* the backend has already said which part of opening the output failed */
    aud_preview_stop(p);
    return -1;
  }

  p->buf = malloc((size_t)AUD_PREVIEW_CHUNK * p->wav.channels * sizeof(*p->buf));
  if (p->buf == NULL)
  {
    aud_warn("cannot allocate a preview buffer for %u channels", p->wav.channels);
    aud_preview_stop(p);
    return -1;
  }

  p->latency = cfg.period_frames * cfg.periods;
  p->written = 0;
  p->playing = 1;
  snprintf(p->path, sizeof(p->path), "%s", path);
  return 0;
}

int aud_preview_pump(aud_preview *p)
{
  long space;

  if (p == NULL || !p->playing || p->out == NULL)
  {
    return 0;
  }

  space = aud_monitor_space(p->out);
  if (space < 0)
  {
    aud_warn("the preview stopped: the output stream failed");
    aud_preview_stop(p);
    return 1;
  }

  while (space > 0)
  {
    size_t want = (size_t)space;
    long got;

    if (want > AUD_PREVIEW_CHUNK)
    {
      want = AUD_PREVIEW_CHUNK;
    }

    got = wav_read_frames(&p->wav, p->buf, want);
    if (got < 0)
    {
      aud_warn("cannot read %s: %s", p->path,
               p->wav.error != NULL ? p->wav.error : "read failed");
      aud_preview_stop(p);
      return 1;
    }
    if (got == 0)
    {
      break; /* the end of the file; the tail of it is still being played */
    }

    if (aud_monitor_write(p->out, p->buf, (size_t)got, 1.0f) != 0)
    {
      aud_warn("the preview stopped: the output stream failed");
      aud_preview_stop(p);
      return 1;
    }

    p->written += (uint64_t)got;
    space -= got;
  }

  /*
   * Done only once what was handed over has actually been played. Closing the
   * output at the last frame written would cut a buffer's worth off the end of
   * every take, which on a preview is the part being listened for.
   */
  if (p->wav.position >= p->wav.frames &&
      p->written >= p->wav.frames + (uint64_t)p->latency)
  {
    aud_preview_stop(p);
    return 1;
  }

  return 0;
}

int aud_preview_playing(const aud_preview *p)
{
  return p != NULL && p->playing;
}

double aud_preview_position(const aud_preview *p)
{
  uint64_t played;

  if (p == NULL || !p->playing || p->wav.rate == 0)
  {
    return 0.0;
  }

  /*
   * What has been handed over, less what the output is still holding. Out by
   * less than one buffer and in the right direction, which is what a readout
   * that is meant to agree with what is being heard needs.
   */
  played = p->written > p->latency ? p->written - p->latency : 0;
  if (played > p->wav.frames)
  {
    played = p->wav.frames;
  }

  return (double)played / p->wav.rate;
}

double aud_preview_length(const aud_preview *p)
{
  if (p == NULL || !p->open)
  {
    return 0.0;
  }
  return wav_read_duration(&p->wav);
}

const char *aud_preview_path(const aud_preview *p)
{
  return p != NULL ? p->path : "";
}
