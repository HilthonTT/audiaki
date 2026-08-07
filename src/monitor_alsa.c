/* SPDX-License-Identifier: MIT */
/*
 * monitor_alsa.c - the ALSA playback backend, for monitoring.
 *
 * The second of the two translation units that include <alsa/asoundlib.h>.
 * Contract and semantics are monitor.h's; this is how ALSA meets them.
 */
#include "backend.h"
#include "log.h"
#include "monitor.h"

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

typedef struct
{
  snd_pcm_t *pcm;
  unsigned rate;
  unsigned channels;
  snd_pcm_uframes_t period_frames;
  snd_pcm_uframes_t buffer_frames;
  int16_t *stage;      /* period_frames * channels samples */
  size_t stage_frames; /* frames the staging buffer holds */
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
  if (rate != cfg->rate)
  {
    /*
     * Resampling here would mean carrying an interpolator around for a
     * convenience feature. Refusing is honest, and "default" almost always
     * accepts whatever the capture side negotiated. The PipeWire backend does
     * not have this limitation: the server resamples as a matter of course.
     */
    aud_warn("monitor: output wants %u Hz but the capture is %u Hz, not monitoring", rate,
             cfg->rate);
    aud_info("the pipewire backend monitors at any rate: --backend pipewire");
    return -1;
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

static int alsa_monitor_write(void *impl, const float *interleaved, size_t frames,
                              float gain)
{
  alsa_monitor *m = impl;
  snd_pcm_sframes_t avail;

  if (m == NULL || m->failed)
  {
    return -1;
  }
  if (interleaved == NULL || frames == 0)
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

    interleaved += (size_t)wrote * m->channels;
    frames -= (size_t)wrote;
  }

  return 0;
}

const aud_monitor_ops aud_monitor_ops_alsa = {
    .name = "alsa",
    .open = alsa_monitor_open,
    .close = alsa_monitor_close,
    .write = alsa_monitor_write,
    .dropped = alsa_monitor_dropped,
};
