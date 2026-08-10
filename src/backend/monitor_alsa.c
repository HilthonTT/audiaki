/* SPDX-License-Identifier: MIT */
/*
 * monitor_alsa.c - the ALSA playback backend, for monitoring.
 *
 * The second of the two translation units that include <alsa/asoundlib.h>.
 * Contract and semantics are monitor.h's; this is how ALSA meets them.
 */
#include "audio/resample.h"
#include "backend/backend.h"
#include "backend/monitor.h"
#include "util/log.h"

/* snd_pcm_hw_params_alloca() expands to alloca(), which strict ISO mode does
 * not declare as a builtin. */
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MONITOR_DEFAULT_PERIOD_FRAMES 512u
#define MONITOR_DEFAULT_PERIODS 3u

/* Input frames converted per pass when the output will not take the rate. */
#define MONITOR_RESAMPLE_CHUNK 1024u

typedef struct
{
  snd_pcm_t *pcm;
  unsigned rate;    /* what the device is actually running at */
  unsigned in_rate; /* what callers hand over, which may not be the same */
  unsigned channels;
  snd_pcm_uframes_t period_frames;
  snd_pcm_uframes_t buffer_frames;
  int16_t *stage;      /* period_frames * channels samples */
  size_t stage_frames; /* frames the staging buffer holds */
  /*
   * Only there when the device would not take the stream's rate. NULL is the
   * ordinary case and the write path costs nothing extra for it.
   */
  aud_resampler *resampler;
  float *converted; /* output of the converter, interleaved */
  size_t converted_cap;
  unsigned long dropped;
  int failed;
} alsa_monitor;

/*
 * S16_LE for the output regardless of what the interface captured. The monitor
 * exists to be heard, not measured, and 16 bit is the one format every playback
 * device accepts - including the PulseAudio and PipeWire plugin devices, which
 * is what "default" usually resolves to.
 */
static int configure(alsa_monitor *m, const aud_monitor_config *cfg)
{
  snd_pcm_hw_params_t *hw = NULL;
  snd_pcm_sw_params_t *sw = NULL;
  snd_pcm_uframes_t period = cfg->period_frames;
  snd_pcm_uframes_t buffer;
  unsigned rate = cfg->rate;
  unsigned periods = cfg->periods > 0 ? cfg->periods : MONITOR_DEFAULT_PERIODS;
  int dir = 0;
  int err;

  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_sw_params_alloca(&sw);

  if ((err = snd_pcm_hw_params_any(m->pcm, hw)) < 0)
  {
    aud_warn("monitor: no playback configurations available: %s", snd_strerror(err));
    return -1;
  }

  err = snd_pcm_hw_params_set_access(m->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
  {
    aud_warn("monitor: no interleaved playback access: %s", snd_strerror(err));
    return -1;
  }

  if ((err = snd_pcm_hw_params_set_format(m->pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0)
  {
    aud_warn("monitor: output does not accept 16-bit samples: %s", snd_strerror(err));
    return -1;
  }

  if ((err = snd_pcm_hw_params_set_channels(m->pcm, hw, cfg->channels)) < 0)
  {
    aud_warn("monitor: output does not accept %u channel(s): %s", cfg->channels,
             snd_strerror(err));
    return -1;
  }

  if ((err = snd_pcm_hw_params_set_rate_near(m->pcm, hw, &rate, &dir)) < 0)
  {
    aud_warn("monitor: cannot set %u Hz playback: %s", cfg->rate, snd_strerror(err));
    return -1;
  }
  /*
   * A device that will not take the stream's rate used to be the end of it.
   * Now the difference is converted on the way out - see audio/resample.h - so
   * an interface that only offers 48 kHz can still monitor a 44.1 kHz take.
   * The file is untouched either way: this is the playback path.
   */
  if (rate != cfg->rate)
  {
    aud_info("monitor: output wants %u Hz and the audio is %u Hz; converting on the "
             "way out",
             rate, cfg->rate);
  }

  dir = 0;
  if ((err = snd_pcm_hw_params_set_period_size_near(m->pcm, hw, &period, &dir)) < 0)
  {
    aud_warn("monitor: cannot set the playback period: %s", snd_strerror(err));
    return -1;
  }

  buffer = period * periods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(m->pcm, hw, &buffer)) < 0)
  {
    aud_warn("monitor: cannot set the playback buffer: %s", snd_strerror(err));
    return -1;
  }

  if ((err = snd_pcm_hw_params(m->pcm, hw)) < 0)
  {
    aud_warn("monitor: cannot apply playback parameters: %s", snd_strerror(err));
    return -1;
  }

  /* re-read: the driver may have rounded anything we asked for */
  snd_pcm_hw_params_get_period_size(hw, &period, &dir);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);

  if ((err = snd_pcm_sw_params_current(m->pcm, sw)) < 0)
  {
    aud_warn("monitor: cannot read the software parameters: %s", snd_strerror(err));
    return -1;
  }

  /*
   * Start as soon as one period is queued rather than waiting for a full
   * buffer, so switching monitoring on is heard immediately instead of after
   * the buffer's worth of silence.
   */
  snd_pcm_sw_params_set_start_threshold(m->pcm, sw, period);
  snd_pcm_sw_params_set_avail_min(m->pcm, sw, period);

  if ((err = snd_pcm_sw_params(m->pcm, sw)) < 0)
  {
    aud_warn("monitor: cannot apply the software parameters: %s", snd_strerror(err));
    return -1;
  }

  if ((err = snd_pcm_prepare(m->pcm)) < 0)
  {
    aud_warn("monitor: cannot prepare playback: %s", snd_strerror(err));
    return -1;
  }

  m->rate = rate;
  m->in_rate = cfg->rate;
  m->channels = cfg->channels;
  m->period_frames = period;
  m->buffer_frames = buffer;
  return 0;
}

static void alsa_monitor_close(void *impl)
{
  alsa_monitor *m = impl;

  if (m == NULL)
  {
    return;
  }

  if (m->pcm != NULL)
  {
    snd_pcm_drop(m->pcm);
    snd_pcm_close(m->pcm);
  }
  free(m->stage);
  aud_resample_destroy(m->resampler);
  free(m->converted);
  free(m);
}

static void *alsa_monitor_open(const aud_monitor_config *cfg, unsigned *rate_out,
                               unsigned *channels_out)
{
  const char *name = cfg->name != NULL ? cfg->name : AUD_MONITOR_DEFAULT_DEVICE;
  alsa_monitor *m;
  int err;

  m = calloc(1, sizeof(*m));
  if (m == NULL)
  {
    aud_warn("monitor: out of memory");
    return NULL;
  }

  /* Non-blocking so a stalled output can never hold up the capture thread. */
  err = snd_pcm_open(&m->pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0)
  {
    aud_warn("monitor: cannot open playback device '%s': %s", name, snd_strerror(err));
    if (err == -EBUSY)
    {
      aud_info("the output is held exclusively by another program");
    }
    free(m);
    return NULL;
  }

  if (configure(m, cfg) != 0)
  {
    snd_pcm_close(m->pcm);
    free(m);
    return NULL;
  }

  m->stage_frames = (size_t)m->period_frames;
  m->stage = malloc(m->stage_frames * m->channels * sizeof(*m->stage));
  if (m->stage == NULL)
  {
    aud_warn("monitor: out of memory");
    snd_pcm_close(m->pcm);
    free(m);
    return NULL;
  }

  if (m->rate != m->in_rate)
  {
    m->resampler = aud_resample_create(m->in_rate, m->rate, m->channels);
    if (m->resampler != NULL)
    {
      m->converted_cap = aud_resample_out_max(m->resampler, MONITOR_RESAMPLE_CHUNK);
      m->converted = malloc(m->converted_cap * m->channels * sizeof(*m->converted));
    }
    if (m->resampler == NULL || m->converted == NULL)
    {
      /*
       * Without the converter there is nothing sensible to play: the device is
       * running at a rate the audio is not. Better to say so than to play it
       * at the wrong pitch.
       */
      aud_warn("monitor: cannot convert %u Hz to %u Hz, not playing it", m->in_rate,
               m->rate);
      alsa_monitor_close(m);
      return NULL;
    }
  }

  aud_debug("monitor: %s, %u Hz, %u ch, period %lu frames, buffer %lu frames (%.1f ms)",
            name, m->rate, m->channels, (unsigned long)m->period_frames,
            (unsigned long)m->buffer_frames,
            1000.0 * (double)m->buffer_frames / (double)m->rate);

  *rate_out = m->rate;
  *channels_out = m->channels;
  return m;
}

static unsigned long alsa_monitor_dropped(const void *impl)
{
  const alsa_monitor *m = impl;

  return m != NULL ? m->dropped : 0;
}

/* Scale, clip and narrow one staging buffer's worth of frames to S16_LE. */
static void stage_samples(int16_t *dst, const float *src, size_t samples, float gain)
{
  for (size_t i = 0; i < samples; i++)
  {
    float v = src[i] * gain;

    if (v > 1.0f)
    {
      v = 1.0f;
    }
    else if (v < -1.0f)
    {
      v = -1.0f;
    }

    /*
     * 32767 rather than 32768: scaling by the negative full scale would let a
     * sample at exactly +1.0 wrap to the most negative value.
     */
    dst[i] = (int16_t)(v * 32767.0f);
  }
}

/* Returns 0 if the stream is usable again, -1 if it is finished. */
static int recover(alsa_monitor *m, int err)
{
  if (snd_pcm_recover(m->pcm, err, 1 /* silent */) == 0)
  {
    return 0;
  }

  aud_warn("monitor: playback failed: %s", snd_strerror(err));
  m->failed = 1;
  return -1;
}

/*
 * Hand frames already at the device's own rate to the device. The rate
 * conversion, when there is one, happens in the caller.
 */
static int push_frames(alsa_monitor *m, const float *interleaved, size_t frames,
                       float gain)
{
  snd_pcm_sframes_t avail;

  if (frames == 0)
  {
    return 0;
  }

  avail = snd_pcm_avail_update(m->pcm);
  if (avail < 0)
  {
    if (recover(m, (int)avail) != 0)
    {
      return -1;
    }
    avail = snd_pcm_avail_update(m->pcm);
    if (avail < 0)
    {
      return 0;
    } /* try again with the next period */
  }

  /*
   * Play what fits and drop the rest. Trimming the tail rather than the head
   * keeps the monitor in step with the input: the alternative, letting the
   * queue grow, would put the sound further behind the strings every second.
   */
  if ((size_t)avail < frames)
  {
    m->dropped += (unsigned long)(frames - (size_t)avail);
    frames = (size_t)avail;
  }

  while (frames > 0)
  {
    size_t chunk = frames < m->stage_frames ? frames : m->stage_frames;
    snd_pcm_sframes_t wrote;

    stage_samples(m->stage, interleaved, chunk * m->channels, gain);

    wrote = snd_pcm_writei(m->pcm, m->stage, (snd_pcm_uframes_t)chunk);
    if (wrote < 0)
    {
      if (wrote == -EAGAIN)
      {
        /* the space avail_update promised went away; skip this much */
        m->dropped += (unsigned long)frames;
        return 0;
      }
      if (recover(m, (int)wrote) != 0)
      {
        return -1;
      }
      m->dropped += (unsigned long)frames;
      return 0;
    }

    /*
     * A write that took nothing is not an error and not progress either. Going
     * round again would spin here forever, on the capture thread, with the take
     * behind it - so it counts as dropped like any other frames the output
     * would not take.
     */
    if (wrote == 0)
    {
      m->dropped += (unsigned long)frames;
      return 0;
    }

    interleaved += (size_t)wrote * m->channels;
    frames -= (size_t)wrote;
  }

  return 0;
}

static int alsa_monitor_write(void *impl, const float *interleaved, size_t frames,
                              float gain)
{
  alsa_monitor *m = impl;

  if (m == NULL || m->failed)
  {
    return -1;
  }
  if (interleaved == NULL || frames == 0)
  {
    return 0;
  }

  if (m->resampler == NULL)
  {
    return push_frames(m, interleaved, frames, gain);
  }

  /*
   * Converted a bufferful at a time. The converter carries its phase and its
   * tail across calls, so cutting the stream up here is not audible in it -
   * see resample.h.
   */
  while (frames > 0)
  {
    size_t take = frames < MONITOR_RESAMPLE_CHUNK ? frames : MONITOR_RESAMPLE_CHUNK;
    size_t got =
        aud_resample_run(m->resampler, interleaved, take, m->converted, m->converted_cap);

    if (push_frames(m, m->converted, got, gain) != 0)
    {
      return -1;
    }

    interleaved += take * m->channels;
    frames -= take;
  }
  return 0;
}

static long alsa_monitor_space(void *impl)
{
  alsa_monitor *m = impl;
  snd_pcm_sframes_t avail;

  if (m == NULL || m->failed)
  {
    return -1;
  }

  avail = snd_pcm_avail_update(m->pcm);
  if (avail < 0)
  {
    if (recover(m, (int)avail) != 0)
    {
      return -1;
    }
    avail = snd_pcm_avail_update(m->pcm);
    if (avail < 0)
    {
      return 0;
    } /* recovered but still not ready; ask again next time */
  }

  /*
   * Answered in the caller's frames rather than the device's. A caller reads
   * its source and hands it over at the source's rate; telling it how much
   * room there is in the device's frames would have it over-read whenever the
   * output runs faster than the file, and the surplus would be dropped.
   *
   * Rounded down, so the answer is never more than will actually fit.
   */
  if (m->resampler != NULL && m->rate != 0)
  {
    return (long)((uint64_t)avail * m->in_rate / m->rate);
  }
  return (long)avail;
}

static void alsa_monitor_drain(void *impl)
{
  alsa_monitor *m = impl;

  if (m == NULL || m->failed)
  {
    return;
  }

  /*
   * The PCM is non-blocking so that a stalled output can never hold up a
   * capture thread, and snd_pcm_drain() on a non-blocking PCM returns at once
   * and finishes in the background - which is the one thing a caller waiting
   * for the tail does not want. Blocking for the drain is safe because nothing
   * will be written afterwards, and SIGINT still breaks it: the handlers are
   * installed without SA_RESTART.
   */
  if (snd_pcm_nonblock(m->pcm, 0) != 0)
  {
    return;
  }
  snd_pcm_drain(m->pcm);
  snd_pcm_nonblock(m->pcm, 1);
}

static void alsa_monitor_flush(void *impl)
{
  alsa_monitor *m = impl;

  if (m == NULL || m->failed)
  {
    return;
  }

  /*
   * Drop rather than drain: the queued audio is the audio being jumped away
   * from, and playing it out is exactly what the caller asked not to happen.
   * The stream stops when it is dropped, so it has to be prepared again before
   * the next write.
   */
  snd_pcm_drop(m->pcm);
  if (snd_pcm_prepare(m->pcm) < 0)
  {
    /* not fatal on its own: the next write recovers or reports it */
    return;
  }

  /*
   * The converter is holding the tail of what was playing in its filter. Left
   * alone it would smear that across the first frames after the jump, which is
   * the one artefact a seek must not have.
   */
  aud_resample_reset(m->resampler);
}

const aud_monitor_ops aud_monitor_ops_alsa = {
    .name = "alsa",
    .open = alsa_monitor_open,
    .close = alsa_monitor_close,
    .write = alsa_monitor_write,
    .dropped = alsa_monitor_dropped,
    .space = alsa_monitor_space,
    .drain = alsa_monitor_drain,
    .flush = alsa_monitor_flush,
};
