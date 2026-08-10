/* SPDX-License-Identifier: MIT */
/*
 * monitor_jack.c - the JACK playback backend, for monitoring.
 *
 * The capture side's inversion the other way round: the caller pushes frames
 * whenever a period has been captured, and the server pulls them when the graph
 * runs. A lock-free FIFO between the two absorbs it, for the same reason
 * device_jack.c uses one - the process callback is real-time, and a monitor is
 * not worth an xrun in every other client on the machine.
 *
 * JACK does not resample. That makes this the one backend where playing a
 * 44.1 kHz take on a graph running at 48 kHz is audiaki's problem rather than
 * the server's, so audio/resample.h is used on the way out, exactly as the ALSA
 * monitor uses it for an output that will not take the capture rate.
 */
#include "audio/resample.h"
#include "backend/backend.h"
#include "backend/jack_common.h"
#include "backend/monitor.h"
#include "util/log.h"
#include "util/ringbuf.h"
#include "version.h"

#include <jack/jack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Input frames converted per pass when the graph is not at the take's rate. */
#define JACK_MONITOR_RESAMPLE_CHUNK 1024u

/*
 * How much to hold. The capture side hands over a period at a time and the
 * server asks for its own period at a time; neither is the other's size, so the
 * FIFO needs room for the mismatch without being deep enough to put the monitor
 * audibly behind the strings. About 100 ms at 48 kHz, and never less than a few
 * of the server's periods.
 */
#define JACK_MONITOR_FIFO_FRAMES 4800u
#define JACK_MONITOR_FIFO_PERIODS 4u

typedef struct
{
  jack_client_t *client;
  jack_port_t **ports; /* one output port per channel */
  unsigned channels;
  unsigned rate;    /* what the graph is running at */
  unsigned in_rate; /* what callers hand over, which may not be the same */

  aud_ringbuf fifo; /* interleaved float, `channels` per frame */

  /* staging for one server period, so the callback de-interleaves out of one run */
  float *staging;
  size_t staging_frames;

  /*
   * Only there when the graph is not at the caller's rate. NULL is the ordinary
   * case and the write path costs nothing extra for it.
   */
  aud_resampler *resampler;
  float *converted;
  size_t converted_cap;

  /* scaled and clipped on the way in, a chunk at a time */
  float *scaled;
  size_t scaled_cap;

  unsigned long dropped;
  int failed;
  /*
   * Set by flush() and cleared by the callback. Only the ring's reader may move
   * its read index, and the callback is it, so a seek asks rather than does.
   */
  int flushing;
} jack_monitor;

static int monitor_process(jack_nframes_t nframes, void *arg)
{
  jack_monitor *m = arg;
  size_t want = (size_t)nframes;
  size_t got;

  if (want == 0)
  {
    return 0;
  }

  if (m->flushing)
  {
    aud_ringbuf_skip(&m->fifo, aud_ringbuf_available(&m->fifo));
    m->flushing = 0;
  }

  if (want > m->staging_frames)
  {
    want = m->staging_frames; /* the period grew; the rest is silence this cycle */
  }

  got = aud_ringbuf_read(&m->fifo, m->staging, want * m->channels) / m->channels;

  for (unsigned ch = 0; ch < m->channels; ch++)
  {
    float *dst = jack_port_get_buffer(m->ports[ch], nframes);

    if (dst == NULL)
    {
      continue;
    }

    for (size_t i = 0; i < got; i++)
    {
      dst[i] = m->staging[i * m->channels + ch];
    }

    /*
     * Nothing left to play. Silence rather than a repeat of the last buffer:
     * monitoring is off, or paused, or the capture has not caught up, and a
     * held note would be a worse answer than a gap in any of those.
     */
    memset(dst + got, 0, ((size_t)nframes - got) * sizeof(*dst));
  }

  return 0;
}

static void monitor_shutdown(void *arg)
{
  jack_monitor *m = arg;

  m->failed = 1;
}

/*
 * The graph moved to another rate. The converter was built for the old one and
 * the FIFO was sized from it, so the monitor stops rather than playing the take
 * at the wrong pitch. A monitor going quiet is not fatal to a recording.
 */
static int monitor_srate(jack_nframes_t nframes, void *arg)
{
  jack_monitor *m = arg;

  if ((unsigned)nframes != m->rate)
  {
    aud_warn("monitor: the jack server moved to %u Hz; not playing through it",
             (unsigned)nframes);
    m->failed = 1;
  }
  return 0;
}

/* Runs with the graph stopped, which is where a JACK client may allocate. */
static int monitor_bufsize(jack_nframes_t nframes, void *arg)
{
  jack_monitor *m = arg;
  float *staging;

  if ((size_t)nframes <= m->staging_frames)
  {
    return 0;
  }

  staging = realloc(m->staging, (size_t)nframes * m->channels * sizeof(*staging));
  if (staging == NULL)
  {
    /* the callback clamps to staging_frames, so this costs a shorter buffer */
    aud_warn("monitor: out of memory resizing for a %u frame period", (unsigned)nframes);
    return -1;
  }

  m->staging = staging;
  m->staging_frames = (size_t)nframes;
  return 0;
}

static void monitor_teardown(jack_monitor *m)
{
  if (m == NULL)
  {
    return;
  }

  /* the client owns the thread that runs the callback; closing it joins that */
  if (m->client != NULL)
  {
    jack_client_close(m->client);
    m->client = NULL;
  }

  aud_ringbuf_free(&m->fifo);
  aud_resample_destroy(m->resampler);
  free(m->ports);
  free(m->staging);
  free(m->converted);
  free(m->scaled);
  free(m);
}

static void jack_monitor_close(void *impl)
{
  monitor_teardown((jack_monitor *)impl);
}

/*
 * Wire the output ports to the sink the caller named, or to the graph's own
 * playback ports when it named none. Returns the number connected.
 */
static int connect_sinks(jack_monitor *m, const char *name)
{
  int is_default = name == NULL || strcmp(name, AUD_MONITOR_DEFAULT_DEVICE) == 0;
  unsigned long flags = JackPortIsInput | (is_default ? JackPortIsPhysical : 0ul);
  const char **ports = jack_get_ports(m->client, NULL, JACK_DEFAULT_AUDIO_TYPE, flags);
  unsigned connected = 0;

  if (ports == NULL)
  {
    return 0;
  }

  for (int i = 0; ports[i] != NULL && connected < m->channels; i++)
  {
    if (!is_default && !aud_jack_port_is_client(ports[i], name))
    {
      continue;
    }
    if (aud_jack_port_is_ours(ports[i]))
    {
      continue; /* audiaki's own capture input is not somewhere to play */
    }

    if (jack_connect(m->client, jack_port_name(m->ports[connected]), ports[i]) != 0)
    {
      aud_warn("monitor: cannot connect to %s", ports[i]);
      continue;
    }
    aud_debug("monitor: %s -> %s", jack_port_name(m->ports[connected]), ports[i]);
    connected++;
  }

  jack_free(ports);
  return (int)connected;
}

static void *jack_monitor_open(const aud_monitor_config *cfg, unsigned *rate_out,
                               unsigned *channels_out)
{
  const char *name = cfg->name != NULL ? cfg->name : AUD_MONITOR_DEFAULT_DEVICE;
  jack_monitor *m;
  size_t fifo_frames;
  unsigned period;
  int connected;

  m = calloc(1, sizeof(*m));
  if (m == NULL)
  {
    aud_warn("monitor: out of memory");
    return NULL;
  }

  m->channels = cfg->channels;
  m->in_rate = cfg->rate;

  m->client = aud_jack_open_client(JACK_CLIENT_PREFIX "-monitor", 1 /* quiet */);
  if (m->client == NULL)
  {
    /* the capture side has already said the server is not there, if it is not */
    aud_warn("monitor: cannot join the jack graph");
    free(m);
    return NULL;
  }

  m->rate = (unsigned)jack_get_sample_rate(m->client);
  period = (unsigned)jack_get_buffer_size(m->client);

  m->staging_frames = period;
  m->staging = malloc(m->staging_frames * m->channels * sizeof(*m->staging));
  m->scaled_cap = JACK_MONITOR_RESAMPLE_CHUNK;
  m->scaled = malloc(m->scaled_cap * m->channels * sizeof(*m->scaled));
  m->ports = calloc(m->channels, sizeof(*m->ports));
  if (m->staging == NULL || m->scaled == NULL || m->ports == NULL)
  {
    aud_warn("monitor: out of memory");
    monitor_teardown(m);
    return NULL;
  }

  fifo_frames = JACK_MONITOR_FIFO_FRAMES;
  if (fifo_frames < (size_t)period * JACK_MONITOR_FIFO_PERIODS)
  {
    fifo_frames = (size_t)period * JACK_MONITOR_FIFO_PERIODS;
  }

  if (aud_ringbuf_init(&m->fifo, fifo_frames * m->channels) != 0)
  {
    aud_warn("monitor: out of memory");
    monitor_teardown(m);
    return NULL;
  }

  if (m->rate != m->in_rate)
  {
    /*
     * The graph is at another rate and will not convert, so audiaki does. The
     * file is untouched either way: this is the playback path.
     */
    aud_info("monitor: the jack graph runs at %u Hz and the audio is %u Hz; "
             "converting on the way out",
             m->rate, m->in_rate);

    m->resampler = aud_resample_create(m->in_rate, m->rate, m->channels);
    if (m->resampler != NULL)
    {
      m->converted_cap = aud_resample_out_max(m->resampler, JACK_MONITOR_RESAMPLE_CHUNK);
      m->converted = malloc(m->converted_cap * m->channels * sizeof(*m->converted));
    }
    if (m->resampler == NULL || m->converted == NULL)
    {
      aud_warn("monitor: cannot convert %u Hz to %u Hz, not playing it", m->in_rate,
               m->rate);
      monitor_teardown(m);
      return NULL;
    }
  }

  jack_set_process_callback(m->client, monitor_process, m);
  jack_set_buffer_size_callback(m->client, monitor_bufsize, m);
  jack_set_sample_rate_callback(m->client, monitor_srate, m);
  jack_on_shutdown(m->client, monitor_shutdown, m);

  for (unsigned ch = 0; ch < m->channels; ch++)
  {
    char port_name[32];

    snprintf(port_name, sizeof(port_name), "out_%u", ch + 1u);
    m->ports[ch] = jack_port_register(m->client, port_name, JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsOutput, 0);
    if (m->ports[ch] == NULL)
    {
      aud_warn("monitor: cannot register playback port %s", port_name);
      monitor_teardown(m);
      return NULL;
    }
  }

  if (jack_activate(m->client) != 0)
  {
    aud_warn("monitor: cannot activate the playback client");
    monitor_teardown(m);
    return NULL;
  }

  connected = connect_sinks(m, name);
  if (connected == 0)
  {
    /*
     * Not a failure. The ports exist and are running, and a JACK user who
     * wanted them somewhere else is one qjackctl away from it - which is more
     * than the other backends can offer when their output device is missing.
     */
    aud_warn("monitor: nothing is connected to the playback ports");
  }

  aud_debug("monitor: jack %s, %u Hz, %u ch, server period %u frames, fifo %lu "
            "frames (%.1f ms)",
            name, m->rate, m->channels, period, (unsigned long)fifo_frames,
            1000.0 * (double)fifo_frames / (double)m->rate);

  *rate_out = m->rate;
  *channels_out = m->channels;
  return m;
}

static unsigned long jack_monitor_dropped(const void *impl)
{
  const jack_monitor *m = impl;

  return m != NULL ? m->dropped : 0;
}

/* Frames the FIFO would take right now, in the graph's own frames. */
static size_t fifo_space(const jack_monitor *m)
{
  return aud_ringbuf_space(&m->fifo) / m->channels;
}

/*
 * Hand frames already at the graph's rate to the FIFO, scaling and clipping on
 * the way. The rate conversion, when there is one, happens in the caller.
 */
static void push_frames(jack_monitor *m, const float *interleaved, size_t frames,
                        float gain)
{
  size_t space = fifo_space(m);

  /*
   * Play what fits and drop the rest, trimming the tail. Letting the queue grow
   * would put the sound further behind the strings every second, which is worse
   * than a skip.
   */
  if (space < frames)
  {
    m->dropped += (unsigned long)(frames - space);
    frames = space;
  }

  while (frames > 0)
  {
    size_t chunk = frames < m->scaled_cap ? frames : m->scaled_cap;
    size_t samples = chunk * m->channels;

    for (size_t i = 0; i < samples; i++)
    {
      float v = interleaved[i] * gain;

      /* clipped rather than wrapped, as the ALSA monitor does on the way to S16 */
      if (v > 1.0f)
      {
        v = 1.0f;
      }
      else if (v < -1.0f)
      {
        v = -1.0f;
      }
      m->scaled[i] = v;
    }

    aud_ringbuf_write(&m->fifo, m->scaled, samples);

    interleaved += samples;
    frames -= chunk;
  }
}

static int jack_monitor_write(void *impl, const float *interleaved, size_t frames,
                              float gain)
{
  jack_monitor *m = impl;

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
    push_frames(m, interleaved, frames, gain);
    return 0;
  }

  /*
   * Converted a bufferful at a time. The converter carries its phase and its
   * tail across calls, so cutting the stream up here is not audible in it -
   * see resample.h.
   */
  while (frames > 0)
  {
    size_t take =
        frames < JACK_MONITOR_RESAMPLE_CHUNK ? frames : JACK_MONITOR_RESAMPLE_CHUNK;
    size_t got =
        aud_resample_run(m->resampler, interleaved, take, m->converted, m->converted_cap);

    push_frames(m, m->converted, got, gain);

    interleaved += take * m->channels;
    frames -= take;
  }
  return 0;
}

static long jack_monitor_space(void *impl)
{
  jack_monitor *m = impl;
  size_t space;

  if (m == NULL || m->failed)
  {
    return -1;
  }

  space = fifo_space(m);

  /*
   * Answered in the caller's frames rather than the graph's. A caller reads its
   * source and hands it over at the source's rate; telling it how much room
   * there is in the graph's frames would have it over-read whenever the graph
   * runs faster than the file, and the surplus would be dropped.
   *
   * Rounded down, so the answer is never more than will actually fit.
   */
  if (m->resampler != NULL && m->rate != 0)
  {
    return (long)((unsigned long long)space * m->in_rate / m->rate);
  }
  return (long)space;
}

/* How often the drain looks at the FIFO, and how long it waits for it at all. */
#define JACK_MONITOR_DRAIN_POLL_MS 5
#define JACK_MONITOR_DRAIN_TIMEOUT_MS 2000

/*
 * What to allow the graph for the frames it has already taken: its own period,
 * plus whatever the interface adds after it. A round number rather than a
 * measurement, as the PipeWire monitor's is.
 */
#define JACK_MONITOR_DRAIN_TAIL_MS 60

static void sleep_ms(long ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static void jack_monitor_drain(void *impl)
{
  jack_monitor *m = impl;
  long waited = 0;

  if (m == NULL || m->failed)
  {
    return;
  }

  while (aud_ringbuf_available(&m->fifo) > 0 && !m->failed &&
         waited < JACK_MONITOR_DRAIN_TIMEOUT_MS)
  {
    sleep_ms(JACK_MONITOR_DRAIN_POLL_MS);
    waited += JACK_MONITOR_DRAIN_POLL_MS;
  }

  /* an empty FIFO means the graph has taken everything, not that it has played it */
  sleep_ms(JACK_MONITOR_DRAIN_TAIL_MS);
}

static void jack_monitor_flush(void *impl)
{
  jack_monitor *m = impl;

  if (m == NULL)
  {
    return;
  }

  /*
   * Asked of the callback rather than done here: it is the FIFO's only reader,
   * and that is what makes the ring safe without a lock. One period's
   * worth of audio - a millisecond or two - is heard before it takes effect,
   * against the tenth of a second the FIFO holds.
   *
   * Not counted as dropped. Those are frames the output could not keep up with,
   * which is a problem worth reporting; these were thrown away on purpose
   * because somebody pressed a key.
   */
  m->flushing = 1;

  /*
   * The converter is holding the tail of what was playing in its filter. Left
   * alone it would smear that across the first frames after the jump, which is
   * the one artefact a seek must not have.
   */
  aud_resample_reset(m->resampler);
}

const aud_monitor_ops aud_monitor_ops_jack = {
    .name = "jack",
    .open = jack_monitor_open,
    .close = jack_monitor_close,
    .write = jack_monitor_write,
    .dropped = jack_monitor_dropped,
    .space = jack_monitor_space,
    .drain = jack_monitor_drain,
    .flush = jack_monitor_flush,
};
