/* SPDX-License-Identifier: MIT */
/*
 * monitor_pipewire.c - the PipeWire playback backend, for monitoring.
 *
 * The same inversion as the capture side, the other way round: the caller
 * pushes frames whenever a period has been captured, and the server pulls them
 * when it wants to play something. A FIFO between the two absorbs it.
 *
 * The stream is float, which is what the server works in, and the server
 * resamples. That is the one behavioural difference from the ALSA monitor,
 * which declines to play at all when the output will not take the capture rate
 * rather than carry an interpolator for a convenience feature. Here the
 * interpolator is already running in another process.
 */
#include "backend.h"
#include "log.h"
#include "monitor.h"
#include "version.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Seconds to wait for the server to agree a format before giving up. */
#define PW_MONITOR_TIMEOUT 2

typedef struct
{
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_stream *stream;
  struct spa_hook stream_listener;

  unsigned rate;
  unsigned channels;

  pthread_mutex_t lock;
  float *fifo;        /* interleaved, `channels` per frame */
  size_t fifo_frames; /* allocation, in frames */
  size_t head;        /* read offset, in frames */
  size_t fill;        /* frames held */
  unsigned long dropped;
  int negotiated;
  int failed;
} pw_monitor;

/*
 * How much to hold. The capture side hands over a period at a time and the
 * server asks for a quantum at a time; neither is the other's size, so the FIFO
 * needs room for the mismatch without being deep enough to put the monitor
 * audibly behind the strings. About 100 ms at 48 kHz.
 */
#define PW_MONITOR_FIFO_FRAMES 4800u

static void monitor_on_process(void *userdata)
{
  pw_monitor *m = userdata;
  struct pw_buffer *b;
  struct spa_data *d;
  float *dst;
  size_t want;
  size_t got;
  size_t first;

  b = pw_stream_dequeue_buffer(m->stream);
  if (b == NULL)
  {
    return;
  }

  d = &b->buffer->datas[0];
  dst = d->data;
  if (dst == NULL)
  {
    pw_stream_queue_buffer(m->stream, b);
    return;
  }

  want = d->maxsize / (sizeof(float) * m->channels);
  if (b->requested != 0 && b->requested < want)
  {
    want = b->requested;
  }

  pthread_mutex_lock(&m->lock);

  got = m->fill < want ? m->fill : want;

  first = m->fifo_frames - m->head;
  if (first > got)
  {
    first = got;
  }
  memcpy(dst, m->fifo + m->head * m->channels, first * m->channels * sizeof(float));
  if (got > first)
  {
    memcpy(dst + first * m->channels, m->fifo,
           (got - first) * m->channels * sizeof(float));
  }

  m->head = (m->head + got) % m->fifo_frames;
  m->fill -= got;

  if (got < want)
  {
    /*
     * Nothing left to play. Silence rather than a repeat of the last buffer:
     * monitoring is off, or paused, or the capture has not caught up, and a
     * held note would be a worse answer than a gap in any of those.
     */
    memset(dst + got * m->channels, 0, (want - got) * m->channels * sizeof(float));
  }

  pthread_mutex_unlock(&m->lock);

  d->chunk->offset = 0;
  d->chunk->stride = (int32_t)(sizeof(float) * m->channels);
  d->chunk->size = (uint32_t)(want * sizeof(float) * m->channels);
  b->size = want;

  pw_stream_queue_buffer(m->stream, b);
}

static void monitor_on_param_changed(void *userdata, uint32_t id,
                                     const struct spa_pod *param)
{
  pw_monitor *m = userdata;
  struct spa_audio_info info;

  if (param == NULL || id != SPA_PARAM_Format)
  {
    return;
  }

  spa_zero(info);
  if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
  {
    return;
  }
  if (info.media_type != SPA_MEDIA_TYPE_audio ||
      info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
  {
    return;
  }
  if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
  {
    return;
  }

  m->rate = info.info.raw.rate;
  m->channels = info.info.raw.channels;
  m->negotiated = 1;
  pw_thread_loop_signal(m->loop, false);
}

static void monitor_on_state_changed(void *userdata, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
  pw_monitor *m = userdata;

  (void)old;

  if (state == PW_STREAM_STATE_ERROR)
  {
    aud_warn("monitor: playback failed: %s", error != NULL ? error : "unknown");
    m->failed = 1;
    pw_thread_loop_signal(m->loop, false);
  }
}

static const struct pw_stream_events monitor_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = monitor_on_state_changed,
    .param_changed = monitor_on_param_changed,
    .process = monitor_on_process,
};

static void monitor_teardown(pw_monitor *m)
{
  if (m == NULL)
  {
    return;
  }

  if (m->loop != NULL)
  {
    pw_thread_loop_stop(m->loop);
  }
  if (m->stream != NULL)
  {
    spa_hook_remove(&m->stream_listener);
    pw_stream_destroy(m->stream);
    m->stream = NULL;
  }
  if (m->core != NULL)
  {
    pw_core_disconnect(m->core);
    m->core = NULL;
  }
  if (m->context != NULL)
  {
    pw_context_destroy(m->context);
    m->context = NULL;
  }
  if (m->loop != NULL)
  {
    pw_thread_loop_destroy(m->loop);
    m->loop = NULL;
  }

  pthread_mutex_destroy(&m->lock);
  free(m->fifo);
  free(m);
}

static void pw_monitor_close(void *impl)
{
  monitor_teardown((pw_monitor *)impl);
}

static void *pw_monitor_open(const aud_monitor_config *cfg, unsigned *rate_out,
                             unsigned *channels_out)
{
  uint8_t builder_buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(builder_buffer, sizeof(builder_buffer));
  const struct spa_pod *params[1];
  struct spa_audio_info_raw raw;
  struct pw_properties *props;
  const char *name = cfg->name != NULL ? cfg->name : AUD_MONITOR_DEFAULT_DEVICE;
  pw_monitor *m;
  char latency[64];

  /* the capture backend has already called pw_init(); this is cheap and safe */
  pw_init(NULL, NULL);

  m = calloc(1, sizeof(*m));
  if (m == NULL)
  {
    aud_warn("monitor: out of memory");
    return NULL;
  }

  if (pthread_mutex_init(&m->lock, NULL) != 0)
  {
    aud_warn("monitor: cannot create the playback lock");
    free(m);
    return NULL;
  }

  m->rate = cfg->rate;
  m->channels = cfg->channels;
  m->fifo_frames = PW_MONITOR_FIFO_FRAMES;
  m->fifo = malloc(m->fifo_frames * m->channels * sizeof(*m->fifo));
  if (m->fifo == NULL)
  {
    aud_warn("monitor: out of memory");
    pthread_mutex_destroy(&m->lock);
    free(m);
    return NULL;
  }

  m->loop = pw_thread_loop_new("audiaki-monitor", NULL);
  if (m->loop == NULL)
  {
    aud_warn("monitor: cannot create the playback loop");
    monitor_teardown(m);
    return NULL;
  }

  m->context = pw_context_new(pw_thread_loop_get_loop(m->loop), NULL, 0);
  if (m->context == NULL || pw_thread_loop_start(m->loop) != 0)
  {
    aud_warn("monitor: cannot start the playback loop");
    monitor_teardown(m);
    return NULL;
  }

  pw_thread_loop_lock(m->loop);

  m->core = pw_context_connect(m->context, NULL, 0);
  if (m->core == NULL)
  {
    pw_thread_loop_unlock(m->loop);
    aud_warn("monitor: cannot connect to the server: %s", strerror(errno));
    monitor_teardown(m);
    return NULL;
  }

  snprintf(latency, sizeof(latency), "%u/%u",
           cfg->period_frames > 0 ? cfg->period_frames : 512u, cfg->rate);

  props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                            PW_KEY_MEDIA_ROLE, "Production", PW_KEY_APP_NAME,
                            AUDIAKI_NAME, PW_KEY_NODE_NAME, AUDIAKI_NAME " monitor",
                            PW_KEY_NODE_LATENCY, latency, NULL);
  if (props == NULL)
  {
    pw_thread_loop_unlock(m->loop);
    aud_warn("monitor: out of memory describing the playback stream");
    monitor_teardown(m);
    return NULL;
  }

  if (strcmp(name, AUD_MONITOR_DEFAULT_DEVICE) != 0)
  {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, name);
  }

  m->stream = pw_stream_new(m->core, AUDIAKI_NAME " monitor", props);
  if (m->stream == NULL)
  {
    pw_thread_loop_unlock(m->loop);
    aud_warn("monitor: cannot create the playback stream");
    monitor_teardown(m);
    return NULL;
  }
  pw_stream_add_listener(m->stream, &m->stream_listener, &monitor_stream_events, m);

  spa_zero(raw);
  raw.format = SPA_AUDIO_FORMAT_F32_LE;
  raw.rate = cfg->rate;
  raw.channels = cfg->channels;
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);

  if (pw_stream_connect(m->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params,
                        1) < 0)
  {
    pw_thread_loop_unlock(m->loop);
    aud_warn("monitor: cannot connect the playback stream to '%s'", name);
    monitor_teardown(m);
    return NULL;
  }

  while (!m->negotiated && !m->failed)
  {
    if (pw_thread_loop_timed_wait(m->loop, PW_MONITOR_TIMEOUT) != 0)
    {
      pw_thread_loop_unlock(m->loop);
      aud_warn("monitor: the output did not agree a format within %d s",
               PW_MONITOR_TIMEOUT);
      monitor_teardown(m);
      return NULL;
    }
  }

  pw_thread_loop_unlock(m->loop);

  if (m->failed)
  {
    monitor_teardown(m);
    return NULL;
  }

  aud_debug("monitor: pipewire %s, %u Hz, %u ch, fifo %lu frames (%.1f ms)", name,
            m->rate, m->channels, (unsigned long)m->fifo_frames,
            1000.0 * (double)m->fifo_frames / (double)m->rate);

  *rate_out = m->rate;
  *channels_out = m->channels;
  return m;
}

static unsigned long pw_monitor_dropped(const void *impl)
{
  const pw_monitor *m = impl;

  return m != NULL ? m->dropped : 0;
}

static int pw_monitor_write(void *impl, const float *interleaved, size_t frames,
                            float gain)
{
  pw_monitor *m = impl;
  size_t space;
  size_t take;
  size_t tail;

  if (m == NULL || m->failed)
  {
    return -1;
  }
  if (interleaved == NULL || frames == 0)
  {
    return 0;
  }

  pthread_mutex_lock(&m->lock);

  space = m->fifo_frames - m->fill;
  take = frames < space ? frames : space;

  /*
   * Same rule as the ALSA monitor: play what fits and drop the rest, trimming
   * the tail. Letting the queue grow would put the sound further behind the
   * strings every second, which is worse than a skip.
   */
  if (take < frames)
  {
    m->dropped += (unsigned long)(frames - take);
  }

  tail = (m->head + m->fill) % m->fifo_frames;
  for (size_t i = 0; i < take; i++)
  {
    const float *src = interleaved + i * m->channels;
    float *dst = m->fifo + ((tail + i) % m->fifo_frames) * m->channels;

    for (unsigned ch = 0; ch < m->channels; ch++)
    {
      float v = src[ch] * gain;

      /* clipped rather than wrapped, as the ALSA monitor does on the way to S16 */
      if (v > 1.0f)
      {
        v = 1.0f;
      }
      else if (v < -1.0f)
      {
        v = -1.0f;
      }
      dst[ch] = v;
    }
  }
  m->fill += take;

  pthread_mutex_unlock(&m->lock);
  return 0;
}

static long pw_monitor_space(void *impl)
{
  pw_monitor *m = impl;
  size_t space;

  if (m == NULL || m->failed)
  {
    return -1;
  }

  pthread_mutex_lock(&m->lock);
  space = m->fifo_frames - m->fill;
  pthread_mutex_unlock(&m->lock);

  return (long)space;
}

/* How often the drain looks at the FIFO, and how long it waits for it at all. */
#define PW_MONITOR_DRAIN_POLL_MS 5
#define PW_MONITOR_DRAIN_TIMEOUT_MS 2000

/*
 * What to allow the server for the frames it has already taken. The stream asks
 * for a period of latency at connect time and the server adds its own quantum
 * on top; there is no cheap way to ask how much is still in flight, so this is
 * a generous round number rather than a measurement.
 */
#define PW_MONITOR_DRAIN_TAIL_MS 150

static void sleep_ms(long ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static void pw_monitor_drain(void *impl)
{
  pw_monitor *m = impl;
  long waited = 0;

  if (m == NULL || m->failed)
  {
    return;
  }

  for (;;)
  {
    size_t fill;

    pthread_mutex_lock(&m->lock);
    fill = m->fill;
    pthread_mutex_unlock(&m->lock);

    if (fill == 0 || m->failed || waited >= PW_MONITOR_DRAIN_TIMEOUT_MS)
    {
      break;
    }

    sleep_ms(PW_MONITOR_DRAIN_POLL_MS);
    waited += PW_MONITOR_DRAIN_POLL_MS;
  }

  /* an empty FIFO means the server has taken everything, not that it has played it */
  sleep_ms(PW_MONITOR_DRAIN_TAIL_MS);
}

const aud_monitor_ops aud_monitor_ops_pipewire = {
    .name = "pipewire",
    .open = pw_monitor_open,
    .close = pw_monitor_close,
    .write = pw_monitor_write,
    .dropped = pw_monitor_dropped,
    .space = pw_monitor_space,
    .drain = pw_monitor_drain,
};
