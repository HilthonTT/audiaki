/* SPDX-License-Identifier: MIT */
#include "engine.h"

#include "device.h"
#include "log.h"
#include "monitor.h"
#include "preroll.h"
#include "ringbuf.h"
#include "wav.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Seconds of audio the visualiser ring holds before the oldest is dropped. */
#define ENGINE_VISUAL_SECONDS 1.0

/* Monitoring gain is carried between threads as an integer in thousandths. */
#define ENGINE_GAIN_SCALE 1000
#define ENGINE_GAIN_MAX 2000 /* +6 dB */

struct aud_engine
{
  aud_device dev;
  pthread_t thread;
  pthread_mutex_t lock;
  int thread_started;

  atomic_int running;      /* cleared to ask the capture thread to finish */
  atomic_int monitor_want; /* the UI's intent, applied by the capture thread */
  atomic_int monitor_gain; /* thousandths */

  /* -- protected by lock -------------------------------------------------- */
  aud_engine_state state;
  wav_writer wav;
  int take_open; /* a WAV file is open and needs closing */
  char path[AUD_ENGINE_PATH_MAX];
  char error[AUD_ENGINE_ERROR_MAX];
  uint64_t frames;
  double peak;
  unsigned xruns;
  int clipped;
  int monitoring;
  unsigned long monitor_dropped;
  int flush_preroll;     /* the take just started and has yet to be given its lead */
  size_t preroll_frames; /* what the ring holds, republished for the UI */

  /* -- capture thread only ------------------------------------------------ */
  unsigned char *hw_buf;
  unsigned char *out_buf; /* aliases hw_buf when no repack is needed */
  float *mono;            /* period_frames, for the visualiser */
  float *inter;           /* period_frames * channels, for the monitor */
  aud_monitor *monitor;
  const char *monitor_device;
  int repack;
  unsigned wav_bytes;

  /*
   * The seconds before the button was pressed. Filling and emptying it both
   * happen here, so the buffer needs no lock of its own: the UI only raises
   * flush_preroll and reads the published preroll_frames.
   */
  aud_preroll preroll;

  /* written by the capture thread, drained by whoever draws */
  aud_ringbuf visual;
};

void aud_engine_config_defaults(aud_engine_config *cfg)
{
  aud_device_config dev;

  if (cfg == NULL)
  {
    return;
  }

  aud_device_config_defaults(&dev);
  memset(cfg, 0, sizeof(*cfg));
  cfg->device = dev.name;
  cfg->rate = dev.rate;
  cfg->channels = dev.channels;
  cfg->format = dev.format;
  cfg->period_frames = dev.period_frames;
  cfg->periods = dev.periods;
  cfg->monitor_device = NULL;
}

/* Call with the lock held. */
static void set_error(aud_engine *e, const char *fmt, ...) AUD_PRINTF(2, 3);

static void set_error(aud_engine *e, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(e->error, sizeof(e->error), fmt, ap);
  va_end(ap);
}

/* Call with the lock held; a no-op when no take is open. */
static int close_take(aud_engine *e)
{
  int rc;

  if (!e->take_open)
  {
    return 0;
  }

  e->take_open = 0;
  rc = wav_close(&e->wav);
  if (rc != 0)
  {
    set_error(e, "cannot finalise %s: %s", e->path, strerror(errno));
  }
  return rc;
}

/*
 * Bring the monitor into line with what the UI asked for. Runs on the capture
 * thread so that every libasound call for the playback stream comes from one
 * place, the same way the capture stream is only ever touched here.
 */
static void sync_monitor(aud_engine *e)
{
  int want = atomic_load_explicit(&e->monitor_want, memory_order_relaxed);

  if (want && e->monitor == NULL)
  {
    aud_monitor_config cfg;

    aud_monitor_config_defaults(&cfg, e->dev.rate, e->dev.channels);
    cfg.name = e->monitor_device;
    cfg.period_frames = (unsigned)e->dev.period_frames;

    e->monitor = aud_monitor_open(&cfg);
    if (e->monitor == NULL)
    {
      /* clear the intent so a dead output is not retried every period */
      atomic_store_explicit(&e->monitor_want, 0, memory_order_relaxed);
    }

    pthread_mutex_lock(&e->lock);
    e->monitoring = e->monitor != NULL;
    if (e->monitor == NULL)
    {
      set_error(e, "cannot open the monitor output");
    }
    pthread_mutex_unlock(&e->lock);
  }
  else if (!want && e->monitor != NULL)
  {
    aud_monitor_close(e->monitor);
    e->monitor = NULL;

    pthread_mutex_lock(&e->lock);
    e->monitoring = 0;
    pthread_mutex_unlock(&e->lock);
  }
}

static void feed_monitor(aud_engine *e, size_t frames)
{
  float gain;

  if (e->monitor == NULL)
  {
    return;
  }

  gain = (float)atomic_load_explicit(&e->monitor_gain, memory_order_relaxed) /
         (float)ENGINE_GAIN_SCALE;

  aud_format_to_float(e->inter, e->hw_buf, frames, e->dev.channels, e->dev.format);

  if (aud_monitor_write(e->monitor, e->inter, frames, gain) != 0)
  {
    aud_monitor_close(e->monitor);
    e->monitor = NULL;
    atomic_store_explicit(&e->monitor_want, 0, memory_order_relaxed);

    pthread_mutex_lock(&e->lock);
    e->monitoring = 0;
    set_error(e, "monitoring stopped: the output stream failed");
    pthread_mutex_unlock(&e->lock);
  }
}

/*
 * Write what the pre-roll holds to the front of the take, oldest first, in
 * period sized pieces so the repack has somewhere to land. Call with the lock
 * held; returns non-zero when the take had to be abandoned.
 */
static int write_preroll(aud_engine *e)
{
  aud_preroll_segment seg[2];
  unsigned segments = aud_preroll_segments(&e->preroll, seg);
  size_t frame_bytes = (size_t)e->dev.channels * aud_format_hw_bytes(e->dev.format);
  size_t period = (size_t)e->dev.period_frames;

  for (unsigned s = 0; s < segments; s++)
  {
    size_t done = 0;

    while (done < seg[s].frames)
    {
      const unsigned char *src = seg[s].data + done * frame_bytes;
      size_t frames = seg[s].frames - done;
      size_t nbytes;

      if (frames > period)
      {
        frames = period;
      }
      nbytes = frames * e->dev.channels * e->wav_bytes;

      if (wav_would_overflow(&e->wav, nbytes))
      {
        break;
      }

      if (e->repack)
      {
        aud_format_repack(e->out_buf, src, frames * e->dev.channels, e->dev.format);
        src = e->out_buf;
      }

      if (wav_write(&e->wav, src, nbytes) != 0)
      {
        set_error(e, "cannot write to %s: %s", e->path, strerror(errno));
        close_take(e);
        e->state = AUD_ENGINE_IDLE;
        return -1;
      }

      e->frames += frames;
      done += frames;
    }
  }

  aud_preroll_clear(&e->preroll);
  return 0;
}

/*
 * Append one captured period to the take. Call with the lock held; returns
 * non-zero when the take had to be abandoned.
 */
static int write_period(aud_engine *e, size_t frames)
{
  size_t samples = frames * e->dev.channels;
  size_t nbytes = samples * e->wav_bytes;

  if (wav_would_overflow(&e->wav, nbytes))
  {
    set_error(e, "%s reached the 4 GB WAV limit, recording stopped", e->path);
    close_take(e);
    e->state = AUD_ENGINE_IDLE;
    return -1;
  }

  if (e->repack)
  {
    aud_format_repack(e->out_buf, e->hw_buf, samples, e->dev.format);
  }

  if (wav_write(&e->wav, e->out_buf, nbytes) != 0)
  {
    set_error(e, "cannot write to %s: %s", e->path, strerror(errno));
    close_take(e);
    e->state = AUD_ENGINE_IDLE;
    return -1;
  }

  e->frames += frames;
  return 0;
}

static void *capture_thread(void *arg)
{
  aud_engine *e = (aud_engine *)arg;
  unsigned xruns = 0;

  while (atomic_load_explicit(&e->running, memory_order_acquire))
  {
    long got;
    double peak;

    got = aud_device_read(&e->dev, e->hw_buf, e->dev.period_frames, &xruns);
    if (got < 0)
    {
      pthread_mutex_lock(&e->lock);
      /* salvage whatever was recorded before the stream died */
      close_take(e);
      set_error(e, "the capture device stopped responding");
      e->state = AUD_ENGINE_FAILED;
      pthread_mutex_unlock(&e->lock);
      break;
    }
    if (got == 0)
    {
      continue;
    }

    peak = aud_format_peak(e->hw_buf, (size_t)got, e->dev.channels, e->dev.format);

    /*
     * Analyse and monitor the captured bytes rather than the repacked copy:
     * hw_buf is what the device delivered, and out_buf may alias it anyway.
     */
    aud_format_to_mono(e->mono, e->hw_buf, (size_t)got, e->dev.channels, e->dev.format);
    aud_ringbuf_write_overwrite(&e->visual, e->mono, (size_t)got);

    sync_monitor(e);
    feed_monitor(e, (size_t)got);

    pthread_mutex_lock(&e->lock);

    e->peak = peak;
    e->xruns = xruns;
    if (e->state == AUD_ENGINE_RECORDING)
    {
      if (peak >= AUD_CLIP_THRESHOLD)
      {
        e->clipped = 1;
      }
      if (e->flush_preroll)
      {
        e->flush_preroll = 0;
        write_preroll(e);
      }
      if (e->state == AUD_ENGINE_RECORDING)
      {
        write_period(e, (size_t)got);
      }
    }
    /*
     * Only while idle. A period captured during a take is already in the file,
     * and one captured while paused was deliberately left out of it; holding
     * either would mean it appearing twice, or surviving the pause.
     */
    else if (e->state == AUD_ENGINE_IDLE)
    {
      aud_preroll_push(&e->preroll, e->hw_buf, (size_t)got);
    }
    e->preroll_frames = aud_preroll_filled(&e->preroll);
    if (e->monitor != NULL)
    {
      e->monitor_dropped = aud_monitor_dropped(e->monitor);
    }

    pthread_mutex_unlock(&e->lock);
  }

  return NULL;
}

aud_engine *aud_engine_create(const aud_engine_config *cfg)
{
  aud_device_config dev_cfg;
  aud_engine *e;
  size_t hw_bytes;
  size_t period;
  size_t visual_slots;

  if (cfg == NULL)
  {
    errno = EINVAL;
    return NULL;
  }

  e = calloc(1, sizeof(*e));
  if (e == NULL)
  {
    aud_error("out of memory");
    return NULL;
  }

  if (pthread_mutex_init(&e->lock, NULL) != 0)
  {
    aud_perror("cannot create the engine lock");
    free(e);
    return NULL;
  }

  aud_device_config_defaults(&dev_cfg);
  dev_cfg.name = cfg->device;
  dev_cfg.rate = cfg->rate;
  dev_cfg.channels = cfg->channels;
  dev_cfg.format = cfg->format;
  dev_cfg.period_frames = cfg->period_frames;
  dev_cfg.periods = cfg->periods;

  if (aud_device_open_capture(&e->dev, &dev_cfg) != 0)
  {
    goto fail_lock;
  }

  e->monitor_device = cfg->monitor_device;
  e->repack = aud_format_needs_repack(e->dev.format);
  e->wav_bytes = aud_format_wav_bytes(e->dev.format);
  hw_bytes = aud_format_hw_bytes(e->dev.format);
  period = (size_t)e->dev.period_frames;

  if (hw_bytes == 0 || e->wav_bytes == 0 || period == 0)
  {
    aud_error("unsupported capture format");
    goto fail_device;
  }

  e->hw_buf = malloc(period * e->dev.channels * hw_bytes);
  e->out_buf = e->repack ? malloc(period * e->dev.channels * e->wav_bytes) : e->hw_buf;
  e->mono = malloc(period * sizeof(*e->mono));
  e->inter = malloc(period * e->dev.channels * sizeof(*e->inter));

  if (e->hw_buf == NULL || e->out_buf == NULL || e->mono == NULL || e->inter == NULL)
  {
    aud_error("cannot allocate the capture buffers");
    goto fail_buffers;
  }

  visual_slots = (size_t)(ENGINE_VISUAL_SECONDS * (double)e->dev.rate);
  if (visual_slots < period * 4)
  {
    visual_slots = period * 4;
  }
  if (aud_ringbuf_init(&e->visual, visual_slots) != 0)
  {
    aud_error("cannot allocate the display buffer");
    goto fail_buffers;
  }

  if (cfg->preroll > 0.0)
  {
    size_t frames = aud_preroll_frames_for(cfg->preroll, e->dev.rate);

    /* a ring shorter than a period would be emptied by the first read */
    if (frames < period)
    {
      frames = period;
    }

    if (aud_preroll_init(&e->preroll, frames, (size_t)e->dev.channels * hw_bytes) != 0)
    {
      aud_error("cannot hold %.1f s of pre-roll", cfg->preroll);
      goto fail_rings;
    }
  }

  e->state = AUD_ENGINE_IDLE;
  atomic_init(&e->running, 1);
  atomic_init(&e->monitor_want, 0);
  atomic_init(&e->monitor_gain, ENGINE_GAIN_SCALE);

  if (pthread_create(&e->thread, NULL, capture_thread, e) != 0)
  {
    aud_perror("cannot start the capture thread");
    goto fail_rings;
  }
  e->thread_started = 1;

  return e;

fail_rings:
  aud_ringbuf_free(&e->visual);
  aud_preroll_free(&e->preroll);
fail_buffers:
  if (e->repack)
  {
    free(e->out_buf);
  }
  free(e->hw_buf);
  free(e->mono);
  free(e->inter);
fail_device:
  aud_device_close(&e->dev);
fail_lock:
  pthread_mutex_destroy(&e->lock);
  free(e);
  return NULL;
}

void aud_engine_destroy(aud_engine *e)
{
  if (e == NULL)
  {
    return;
  }

  atomic_store_explicit(&e->running, 0, memory_order_release);
  if (e->thread_started)
  {
    pthread_join(e->thread, NULL);
  }

  /* the thread is gone, so the lock is uncontended, but keep the discipline */
  pthread_mutex_lock(&e->lock);
  close_take(e);
  pthread_mutex_unlock(&e->lock);

  aud_monitor_close(e->monitor);
  aud_ringbuf_free(&e->visual);
  aud_preroll_free(&e->preroll);
  aud_device_close(&e->dev);

  if (e->repack)
  {
    free(e->out_buf);
  }
  free(e->hw_buf);
  free(e->mono);
  free(e->inter);

  pthread_mutex_destroy(&e->lock);
  free(e);
}

unsigned aud_engine_rate(const aud_engine *e)
{
  return e != NULL ? e->dev.rate : 0;
}

unsigned aud_engine_channels(const aud_engine *e)
{
  return e != NULL ? e->dev.channels : 0;
}

aud_format aud_engine_format(const aud_engine *e)
{
  return e != NULL ? e->dev.format : AUD_FORMAT_UNKNOWN;
}

const char *aud_engine_device(const aud_engine *e)
{
  return (e != NULL && e->dev.name != NULL) ? e->dev.name : "";
}

int aud_engine_start(aud_engine *e, const char *path, int overwrite)
{
  int rc = -1;

  if (e == NULL || path == NULL || path[0] == '\0')
  {
    return -1;
  }

  pthread_mutex_lock(&e->lock);

  if (e->state == AUD_ENGINE_FAILED)
  {
    set_error(e, "the capture device is no longer running");
    goto out;
  }
  if (e->state != AUD_ENGINE_IDLE)
  {
    set_error(e, "a take is already in progress");
    goto out;
  }

  if (strlen(path) >= sizeof(e->path))
  {
    set_error(e, "that path is too long");
    goto out;
  }
  snprintf(e->path, sizeof(e->path), "%s", path);

  /* e->path, not the caller's buffer: the writer keeps the pointer */
  if (wav_open(&e->wav, e->path, e->dev.rate, (uint16_t)e->dev.channels,
               (uint16_t)aud_format_wav_bits(e->dev.format), overwrite) != 0)
  {
    if (errno == EEXIST)
    {
      set_error(e, "%s already exists", e->path);
    }
    else
    {
      set_error(e, "cannot create %s: %s", e->path, strerror(errno));
    }
    goto out;
  }

  e->take_open = 1;
  e->frames = 0;
  e->clipped = 0;
  e->xruns = 0;
  e->error[0] = '\0';
  /*
   * Left to the capture thread rather than done here: it owns the buffer, and
   * a megabyte of file writes on the UI thread would hold the lock the capture
   * loop needs every period.
   */
  e->flush_preroll = 1;
  e->state = AUD_ENGINE_RECORDING;
  rc = 0;

out:
  pthread_mutex_unlock(&e->lock);
  return rc;
}

void aud_engine_pause(aud_engine *e)
{
  if (e == NULL)
  {
    return;
  }

  pthread_mutex_lock(&e->lock);
  if (e->state == AUD_ENGINE_RECORDING)
  {
    e->state = AUD_ENGINE_PAUSED;
  }
  pthread_mutex_unlock(&e->lock);
}

void aud_engine_resume(aud_engine *e)
{
  if (e == NULL)
  {
    return;
  }

  pthread_mutex_lock(&e->lock);
  if (e->state == AUD_ENGINE_PAUSED)
  {
    e->state = AUD_ENGINE_RECORDING;
  }
  pthread_mutex_unlock(&e->lock);
}

int aud_engine_stop(aud_engine *e)
{
  int rc;

  if (e == NULL)
  {
    return -1;
  }

  pthread_mutex_lock(&e->lock);
  rc = close_take(e);
  if (e->state != AUD_ENGINE_FAILED)
  {
    e->state = AUD_ENGINE_IDLE;
  }
  pthread_mutex_unlock(&e->lock);

  return rc;
}

void aud_engine_status_get(aud_engine *e, aud_engine_status *out)
{
  if (out == NULL)
  {
    return;
  }

  memset(out, 0, sizeof(*out));
  if (e == NULL)
  {
    return;
  }

  pthread_mutex_lock(&e->lock);

  out->state = e->state;
  out->frames = e->frames;
  out->elapsed = e->dev.rate > 0 ? (double)e->frames / (double)e->dev.rate : 0.0;
  out->bytes = e->frames * e->dev.channels * e->wav_bytes;
  out->peak = e->peak;
  out->xruns = e->xruns;
  out->clipped = e->clipped;
  out->monitoring = e->monitoring;
  out->monitor_dropped = e->monitor_dropped;
  if (e->dev.rate > 0)
  {
    out->preroll_held = (double)e->preroll_frames / (double)e->dev.rate;
    out->preroll_size = (double)aud_preroll_capacity(&e->preroll) / (double)e->dev.rate;
  }
  memcpy(out->path, e->path, sizeof(out->path));
  memcpy(out->error, e->error, sizeof(out->error));

  pthread_mutex_unlock(&e->lock);
}

void aud_engine_set_monitor(aud_engine *e, int enabled)
{
  if (e == NULL)
  {
    return;
  }

  atomic_store_explicit(&e->monitor_want, enabled ? 1 : 0, memory_order_relaxed);
}

void aud_engine_set_monitor_gain(aud_engine *e, float gain)
{
  int scaled;

  if (e == NULL)
  {
    return;
  }

  scaled = (int)(gain * (float)ENGINE_GAIN_SCALE + 0.5f);
  if (scaled < 0)
  {
    scaled = 0;
  }
  if (scaled > ENGINE_GAIN_MAX)
  {
    scaled = ENGINE_GAIN_MAX;
  }

  atomic_store_explicit(&e->monitor_gain, scaled, memory_order_relaxed);
}

float aud_engine_monitor_gain(const aud_engine *e)
{
  if (e == NULL)
  {
    return 0.0f;
  }

  return (float)atomic_load_explicit(&e->monitor_gain, memory_order_relaxed) /
         (float)ENGINE_GAIN_SCALE;
}

int aud_engine_monitor_wanted(const aud_engine *e)
{
  if (e == NULL)
  {
    return 0;
  }

  return atomic_load_explicit(&e->monitor_want, memory_order_relaxed);
}

size_t aud_engine_read_visual(aud_engine *e, float *mono, size_t max)
{
  if (e == NULL)
  {
    return 0;
  }

  return aud_ringbuf_read(&e->visual, mono, max);
}
