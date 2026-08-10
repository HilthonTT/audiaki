/* SPDX-License-Identifier: MIT */
/*
 * device_jack.c - the JACK capture backend.
 *
 * Joins a graph rather than opening a device. That is the difference worth
 * knowing about: under ALSA a capture device is a card, and under PipeWire it
 * is a node the server owns, but under JACK it is a set of ports somebody
 * wired up - and they are as likely to belong to another program as to an
 * interface. So a "device" name here is a JACK client name, and -D system is
 * the interface while -D ardour is whatever Ardour is putting out.
 *
 * JACK pushes, at a fixed period the server chose, and audiaki pulls. The same
 * inversion device_pipewire.c absorbs, absorbed the same way: a FIFO between
 * the process callback and aud_device_read(). The difference is that JACK's
 * callback is genuinely real-time - a mutex held too long here is an xrun in
 * every other client on the machine, not just this one - so the FIFO is the
 * wait-free ring in util/ringbuf.h, and the only lock is the one the reader
 * sleeps on. The callback never waits for it: a missed wake-up costs at most
 * the read timeout, and the reader re-checks the ring when it expires anyway.
 *
 * The ring holds float, which is what JACK hands over, and the encoding into
 * the capture format happens in the reader. That keeps the real-time side down
 * to an interleave and a copy.
 *
 * JACK offers nothing to negotiate - no format, no rate, no period. The server
 * decided all three before audiaki started, so a rate the caller asked for that
 * the server is not running at is reported rather than argued with.
 */
#include "backend/backend.h"
#include "backend/device.h"
#include "backend/jack_common.h"
#include "util/jsonout.h"
#include "util/log.h"
#include "util/ringbuf.h"
#include "version.h"

#include <jack/jack.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Nanoseconds aud_device_read() blocks before returning "nothing yet, ask again". */
#define JACK_READ_TIMEOUT_NS 200000000L

/*
 * How much capture to hold when the caller is not reading. Sized from the
 * period the caller asked for, with a floor, and never less than a few of the
 * server's own periods - a FIFO shorter than one would drop part of every
 * buffer that arrives.
 */
#define JACK_FIFO_MIN_FRAMES 8192u
#define JACK_FIFO_MIN_PERIODS 4u

int aud_jack_server_responds(void)
{
  static int cached = -1; /* -1 unknown, 0 no, 1 yes */
  jack_client_t *client;

  if (cached >= 0)
  {
    return cached;
  }

  client = aud_jack_open_client(JACK_CLIENT_PREFIX "-probe", 1 /* quiet */);
  if (client == NULL)
  {
    cached = 0;
    return 0;
  }

  jack_client_close(client);
  cached = 1;
  return 1;
}

/* -- the ports the graph is offering --------------------------------------- */

/*
 * The audio ports a capture stream could be fed from.
 *
 * An output port is what audiaki connects an input port to, so the direction
 * reads backwards from what a listing of capture devices suggests: the
 * interface's microphone arrives on system:capture_1, which is an output as far
 * as the graph is concerned.
 *
 * The caller frees the result with jack_free().
 */
static const char **source_ports(jack_client_t *client, int physical_only)
{
  unsigned long flags = JackPortIsOutput | (physical_only ? JackPortIsPhysical : 0ul);

  return jack_get_ports(client, NULL, JACK_DEFAULT_AUDIO_TYPE, flags);
}

static int jack_enumerate(aud_device_entry **out)
{
  jack_client_t *client;
  const char **ports;
  aud_device_entry *list = NULL;
  int found = 0;

  if (out == NULL)
  {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;

  client = aud_jack_open_client(JACK_CLIENT_PREFIX "-enum", 0);
  if (client == NULL)
  {
    return -1;
  }

  ports = source_ports(client, 0 /* every client, not only the interfaces */);
  if (ports == NULL)
  {
    jack_client_close(client);
    return 0;
  }

  /*
   * One entry per client rather than per port: a device is the thing you name
   * with -D, and audiaki takes as many of its ports as it has channels. The
   * ports arrive grouped by client already, but this does not rely on it.
   */
  for (int i = 0; ports[i] != NULL; i++)
  {
    size_t len = aud_jack_client_part(ports[i]);
    aud_device_entry *grown;
    int existing = -1;

    if (len == 0 || len >= sizeof(list[0].name) || aud_jack_port_is_ours(ports[i]))
    {
      continue;
    }

    for (int j = 0; j < found; j++)
    {
      if (aud_jack_port_is_client(ports[i], list[j].name))
      {
        existing = j;
        break;
      }
    }

    if (existing >= 0)
    {
      /* a second port for a client already listed: the description is the count */
      unsigned long count = strtoul(list[existing].description, NULL, 10);

      snprintf(list[existing].description, sizeof(list[existing].description),
               "%lu ports", count + 1ul);
      continue;
    }

    grown = realloc(list, (size_t)(found + 1) * sizeof(*list));
    if (grown == NULL)
    {
      aud_error("out of memory listing devices");
      jack_free(ports);
      jack_client_close(client);
      free(list);
      return -1;
    }
    list = grown;

    memset(&list[found], 0, sizeof(list[found]));
    memcpy(list[found].name, ports[i], len);
    list[found].name[len] = '\0';
    snprintf(list[found].card, sizeof(list[found].card), "%s", list[found].name);
    snprintf(list[found].description, sizeof(list[found].description), "1 port");
    found++;
  }

  jack_free(ports);
  jack_client_close(client);

  *out = list;
  return found;
}

/* -- probe ----------------------------------------------------------------- */

/*
 * What a JACK device supports is not a property of the device. The rate and the
 * period belong to the server and are the same for every client on it, and the
 * format is float on the way in whatever the hardware is doing. So this reports
 * the server's terms, says they are the server's, and counts the ports the
 * named client has - which is the one number that does vary, and the one that
 * decides how many channels a recording can have.
 */
static int jack_probe(const char *name, int json)
{
  jack_client_t *client;
  const char **ports;
  const char *device = name != NULL ? name : AUD_DEFAULT_DEVICE;
  int is_default = name == NULL || strcmp(name, AUD_DEFAULT_DEVICE) == 0;
  unsigned rate;
  unsigned period;
  int count = 0;

  client = aud_jack_open_client(JACK_CLIENT_PREFIX "-probe", 0);
  if (client == NULL)
  {
    return -1;
  }

  rate = (unsigned)jack_get_sample_rate(client);
  period = (unsigned)jack_get_buffer_size(client);

  ports = source_ports(client, is_default);
  if (ports != NULL)
  {
    for (int i = 0; ports[i] != NULL; i++)
    {
      if (is_default || aud_jack_port_is_client(ports[i], device))
      {
        count++;
      }
    }
    jack_free(ports);
  }

  if (!is_default && count == 0)
  {
    aud_error("no such capture device '%s'", device);
    aud_info("run '" AUDIAKI_NAME " --list' to see what the graph is offering");
    jack_client_close(client);
    return -1;
  }

  if (json)
  {
    fputs("{\n  \"device\": ", stdout);
    aud_json_string(stdout, device);
    fputs(",\n  \"backend\": \"jack\"", stdout);
    fputs(",\n  \"description\": ", stdout);
    aud_json_string(stdout, is_default ? "the graph's physical capture ports" : device);
    fputs(",\n  \"formats\": [\"S16_LE\", \"S24_3LE\", \"S24_LE\", \"S32_LE\"]", stdout);
    printf(",\n  \"channels\": {\"min\": %u, \"max\": %d}", count > 0 ? 1u : 0u, count);
    printf(",\n  \"rates\": {\"min\": %u, \"max\": %u}", rate, rate);
    printf(",\n  \"period_frames\": {\"min\": %u, \"max\": %u}", period, period);
    fputs(",\n  \"converted\": true\n}\n", stdout);
  }
  else
  {
    printf("device:   %s\n", device);
    printf("backend:  jack\n");
    printf("what:     %s\n",
           is_default ? "the graph's physical capture ports" : "a client on the graph");
    printf("ports:    %d\n", count);
    printf("formats:  S16_LE S24_3LE S24_LE S32_LE\n");
    printf("channels: 1..%d\n", count);
    printf("rates:    %u Hz\n", rate);
    printf("period:   %u frames\n", period);
    printf("\n");
    printf("The rate and the period belong to the server, not to this device, and\n");
    printf("every client on the graph is running at them. JACK works in float, so\n");
    printf("the formats are what audiaki writes rather than what the card does:\n");
    printf("use --backend alsa --probe to ask the hardware.\n");
  }

  jack_client_close(client);
  return 0;
}

/* -- the capture stream ---------------------------------------------------- */

typedef struct
{
  jack_client_t *client;
  jack_port_t **ports; /* one input port per channel */
  unsigned channels;
  unsigned rate;
  aud_format format;
  size_t frame_bytes;

  aud_ringbuf fifo; /* interleaved float, `channels` per frame */

  /*
   * Staging for one server period: JACK hands over a buffer per port, and the
   * ring takes one contiguous run of interleaved samples.
   */
  float *planar;
  size_t planar_frames;

  /* the reader's own, for encoding out of the ring into the capture layout */
  float *decode;
  size_t decode_frames;

  /* only for the reader to sleep on; the callback never waits for it */
  pthread_mutex_t lock;
  pthread_cond_t cond;

  unsigned overruns; /* buffers that did not fit, plus the server's own xruns */
  int broken;
  int stopped; /* set by drop(): stop taking what arrives */
} jack_capture;

/* Wake a reader that is asleep, without ever blocking the real-time thread. */
static void wake_reader(jack_capture *c)
{
  if (pthread_mutex_trylock(&c->lock) != 0)
  {
    /*
     * The reader holds the lock, which means it is awake. It re-checks the ring
     * after taking it, so there is nothing to miss.
     */
    return;
  }
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->lock);
}

static int capture_process(jack_nframes_t nframes, void *arg)
{
  jack_capture *c = arg;
  size_t frames = (size_t)nframes;
  size_t samples;

  if (frames == 0 || c->stopped)
  {
    return 0;
  }

  if (frames > c->planar_frames)
  {
    /* the period grew and the reallocation did not; see capture_bufsize */
    c->overruns++;
    return 0;
  }

  samples = frames * c->channels;

  /*
   * Checked before writing rather than writing what fits: a short write would
   * leave half a frame in the ring, and every frame after it would be read one
   * channel out of step.
   */
  if (aud_ringbuf_space(&c->fifo) < samples)
  {
    /*
     * Nobody is reading fast enough. Dropping what just arrived rather than the
     * backlog keeps the FIFO's contents contiguous in time, and this is counted
     * as an xrun because that is exactly what it is.
     */
    c->overruns++;
    return 0;
  }

  for (unsigned ch = 0; ch < c->channels; ch++)
  {
    const float *src = jack_port_get_buffer(c->ports[ch], nframes);

    for (size_t i = 0; i < frames; i++)
    {
      c->planar[i * c->channels + ch] = src != NULL ? src[i] : 0.0f;
    }
  }

  aud_ringbuf_write(&c->fifo, c->planar, samples);

  wake_reader(c);
  return 0;
}

/*
 * The server changed its period. This runs with the graph stopped, which is the
 * one place a JACK client is allowed to allocate, so the staging buffer is
 * resized here rather than being guessed at a maximum in advance.
 */
static int capture_bufsize(jack_nframes_t nframes, void *arg)
{
  jack_capture *c = arg;
  float *planar;

  if ((size_t)nframes <= c->planar_frames)
  {
    return 0;
  }

  planar = realloc(c->planar, (size_t)nframes * c->channels * sizeof(*planar));
  if (planar == NULL)
  {
    /* the process callback checks planar_frames, so this degrades to silence */
    aud_warn("jack: out of memory resizing for a %u frame period", (unsigned)nframes);
    return -1;
  }

  c->planar = planar;
  c->planar_frames = (size_t)nframes;
  return 0;
}

/*
 * The server changed its rate. Everything above device.h sized its buffers from
 * dev->rate and wrote it into a WAV header, so this cannot be absorbed - the
 * take has to end, the way an unplugged card ends one.
 */
static int capture_srate(jack_nframes_t nframes, void *arg)
{
  jack_capture *c = arg;

  if ((unsigned)nframes != c->rate)
  {
    aud_error("jack: the server moved to %u Hz mid-stream", (unsigned)nframes);
    c->broken = 1;
    wake_reader(c);
  }
  return 0;
}

static int capture_xrun(void *arg)
{
  jack_capture *c = arg;

  /*
   * The server missed its deadline, which means the frames are gone before
   * anything here saw them. Counted where audiaki counts its own overruns: from
   * the caller's side the two are the same event.
   */
  c->overruns++;
  return 0;
}

static void capture_shutdown(void *arg)
{
  jack_capture *c = arg;

  c->broken = 1;
  wake_reader(c);
}

static void capture_free(jack_capture *c)
{
  if (c == NULL)
  {
    return;
  }

  /*
   * Close the client first. It owns the thread that runs the callbacks, and
   * freeing the ring under a callback that is still writing to it is the one
   * race worth being careful about here. jack_client_close() does not return
   * until that thread is gone.
   */
  if (c->client != NULL)
  {
    jack_client_close(c->client);
    c->client = NULL;
  }

  aud_ringbuf_free(&c->fifo);
  pthread_cond_destroy(&c->cond);
  pthread_mutex_destroy(&c->lock);
  free(c->ports);
  free(c->planar);
  free(c->decode);
  free(c);
}

static void jack_close(aud_device *dev)
{
  if (dev->handle == NULL)
  {
    return;
  }

  capture_free((jack_capture *)dev->handle);
  dev->handle = NULL;
}

/*
 * Wire the stream's input ports to the source the caller named. Returns the
 * number connected, or -1 when the name matches nothing at all.
 *
 * Fewer sources than channels is a warning rather than a failure: a mono
 * interface asked for in stereo still records, with silence in the right
 * channel, which is a more useful answer than refusing to start.
 */
static int connect_sources(jack_capture *c, const char *name)
{
  int is_default = name == NULL || strcmp(name, AUD_DEFAULT_DEVICE) == 0;
  const char **ports;
  unsigned connected = 0;
  int matched = 0;

  ports = source_ports(c->client, is_default);
  if (ports == NULL && is_default)
  {
    /* no physical capture ports: a graph with only software on it still works */
    ports = source_ports(c->client, 0);
  }
  if (ports == NULL)
  {
    return is_default ? 0 : -1;
  }

  for (int i = 0; ports[i] != NULL && connected < c->channels; i++)
  {
    if (!is_default && !aud_jack_port_is_client(ports[i], name))
    {
      continue;
    }
    if (aud_jack_port_is_ours(ports[i]))
    {
      continue; /* audiaki's own monitor output is not a capture source */
    }
    matched++;

    if (jack_connect(c->client, ports[i], jack_port_name(c->ports[connected])) != 0)
    {
      aud_warn("jack: cannot connect %s", ports[i]);
      continue;
    }
    aud_debug("jack: %s -> %s", ports[i], jack_port_name(c->ports[connected]));
    connected++;
  }

  jack_free(ports);

  if (!is_default && matched == 0)
  {
    return -1;
  }
  return (int)connected;
}

static int jack_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  jack_capture *c;
  size_t fifo_frames;
  unsigned period;
  int connected;

  c = calloc(1, sizeof(*c));
  if (c == NULL)
  {
    aud_error("out of memory opening the capture stream");
    return -1;
  }

  /*
   * JACK is float end to end and has no integer format to ask for, so the
   * choice is only what audiaki writes into the file. S32_LE by default, which
   * is the one that gives the float it was handed nothing to round to.
   */
  c->format = cfg->format != AUD_FORMAT_UNKNOWN ? cfg->format : AUD_FORMAT_S32_LE;
  c->channels = cfg->channels;
  c->frame_bytes = (size_t)aud_format_hw_bytes(c->format) * cfg->channels;

  if (pthread_mutex_init(&c->lock, NULL) != 0)
  {
    aud_error("cannot create the capture lock");
    free(c);
    return -1;
  }
  if (pthread_cond_init(&c->cond, NULL) != 0)
  {
    aud_error("cannot create the capture condition");
    pthread_mutex_destroy(&c->lock);
    free(c);
    return -1;
  }

  c->client = aud_jack_open_client(JACK_CLIENT_PREFIX, 0);
  if (c->client == NULL)
  {
    capture_free(c);
    return -1;
  }

  c->rate = (unsigned)jack_get_sample_rate(c->client);
  period = (unsigned)jack_get_buffer_size(c->client);

  c->planar_frames = period;
  c->planar = malloc(c->planar_frames * c->channels * sizeof(*c->planar));

  /* the reader asks for its own period, and never gets more than it asked for */
  c->decode_frames = cfg->period_frames > period ? cfg->period_frames : period;
  c->decode = malloc(c->decode_frames * c->channels * sizeof(*c->decode));

  c->ports = calloc(c->channels, sizeof(*c->ports));
  if (c->planar == NULL || c->decode == NULL || c->ports == NULL)
  {
    aud_error("out of memory opening the capture stream");
    capture_free(c);
    return -1;
  }

  fifo_frames = (size_t)cfg->period_frames * cfg->periods;
  if (fifo_frames < JACK_FIFO_MIN_FRAMES)
  {
    fifo_frames = JACK_FIFO_MIN_FRAMES;
  }
  if (fifo_frames < (size_t)period * JACK_FIFO_MIN_PERIODS)
  {
    fifo_frames = (size_t)period * JACK_FIFO_MIN_PERIODS;
  }

  if (aud_ringbuf_init(&c->fifo, fifo_frames * c->channels) != 0)
  {
    aud_error("out of memory sizing the capture buffer");
    capture_free(c);
    return -1;
  }

  jack_set_process_callback(c->client, capture_process, c);
  jack_set_buffer_size_callback(c->client, capture_bufsize, c);
  jack_set_sample_rate_callback(c->client, capture_srate, c);
  jack_set_xrun_callback(c->client, capture_xrun, c);
  jack_on_shutdown(c->client, capture_shutdown, c);

  for (unsigned ch = 0; ch < c->channels; ch++)
  {
    char port_name[32];

    snprintf(port_name, sizeof(port_name), "in_%u", ch + 1u);
    c->ports[ch] = jack_port_register(c->client, port_name, JACK_DEFAULT_AUDIO_TYPE,
                                      JackPortIsInput, 0);
    if (c->ports[ch] == NULL)
    {
      aud_error("jack: cannot register capture port %s", port_name);
      capture_free(c);
      return -1;
    }
  }

  /*
   * Activate before connecting: JACK will not connect the ports of a client
   * that is not running yet, and the callbacks are set above so nothing arrives
   * before there is somewhere to put it.
   */
  if (jack_activate(c->client) != 0)
  {
    aud_error("jack: cannot activate the capture client");
    capture_free(c);
    return -1;
  }

  connected = connect_sources(c, cfg->name);
  if (connected < 0)
  {
    aud_error("no such capture device '%s'", cfg->name);
    aud_info("run '" AUDIAKI_NAME " --list' to see what the graph is offering");
    capture_free(c);
    return -1;
  }
  if (connected == 0)
  {
    aud_warn("jack: nothing is connected to the capture ports; recording silence");
    aud_info("connect them by hand, or name a client with -D");
  }
  else if ((unsigned)connected < c->channels)
  {
    aud_warn("jack: %s has %d port(s) and %u channel(s) were asked for; the rest "
             "will be silence",
             cfg->name != NULL ? cfg->name : AUD_DEFAULT_DEVICE, connected, c->channels);
  }

  dev->handle = c;
  dev->name = cfg->name;
  dev->format = c->format;
  dev->rate = c->rate;
  dev->channels = c->channels;
  dev->period_frames = cfg->period_frames;
  dev->buffer_frames = (unsigned long)fifo_frames;

  if (c->rate != cfg->rate)
  {
    /*
     * Nothing to be done about it: the rate is the server's, it is the same for
     * every client on the graph, and changing it means restarting jackd. Said
     * as a warning because the take will be at this rate whatever was asked for.
     */
    aud_warn("requested %u Hz, the jack server is running at %u Hz", cfg->rate, c->rate);
  }

  aud_debug("opened %s through jack: %s, %u Hz, %u ch, server period %u frames, "
            "fifo %lu frames",
            cfg->name != NULL ? cfg->name : AUD_DEFAULT_DEVICE,
            aud_format_name(dev->format), dev->rate, dev->channels, period,
            dev->buffer_frames);
  return 0;
}

static long jack_read(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns)
{
  jack_capture *c = (jack_capture *)dev->handle;
  size_t wanted = frames < c->decode_frames ? (size_t)frames : c->decode_frames;
  size_t available;
  size_t got;
  unsigned overruns;

  pthread_mutex_lock(&c->lock);

  while (aud_ringbuf_available(&c->fifo) < c->channels && !c->broken)
  {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += JACK_READ_TIMEOUT_NS;
    if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_sec += deadline.tv_nsec / 1000000000L;
      deadline.tv_nsec %= 1000000000L;
    }

    if (pthread_cond_timedwait(&c->cond, &c->lock, &deadline) == ETIMEDOUT)
    {
      /*
       * Nothing arrived. Returning "retry" rather than waiting forever is what
       * lets the caller's loop notice Ctrl+C on a silent or stalled graph.
       */
      pthread_mutex_unlock(&c->lock);
      return 0;
    }
  }

  available = aud_ringbuf_available(&c->fifo) / c->channels;
  pthread_mutex_unlock(&c->lock);

  if (available == 0)
  {
    aud_error("capture stopped: the jack server is no longer there");
    return -1;
  }

  if (available < wanted)
  {
    wanted = available;
  }

  /*
   * Outside the lock. The ring is what makes that safe - one reader, one
   * writer, and nothing shared between them but two atomics - and the lock
   * exists only so the wait above has something to sleep on.
   */
  got = aud_ringbuf_read(&c->fifo, c->decode, wanted * c->channels) / c->channels;
  aud_format_from_float(buf, c->decode, got, c->channels, c->format);

  overruns = c->overruns;
  c->overruns = 0;
  if (overruns > 0 && xruns != NULL)
  {
    *xruns += overruns;
  }

  return (long)got;
}

static void jack_drop(aud_device *dev)
{
  jack_capture *c = (jack_capture *)dev->handle;

  if (c == NULL)
  {
    return;
  }

  /*
   * Stop the callback taking anything more before emptying what it left, so the
   * two do not race over the same ring. Skipping rather than resetting, because
   * only the reader may move the read index and this is the reader.
   */
  c->stopped = 1;
  aud_ringbuf_skip(&c->fifo, aud_ringbuf_available(&c->fifo));
}

/* -- the hotplug watch ----------------------------------------------------- */

/*
 * JACK says when a port or a client appears or goes, so like the PipeWire watch
 * there is nothing to poll and no settling delay to guess at. A client turning
 * up is as much a change as an interface being plugged in here - under JACK the
 * two are the same event, which is rather the point of the graph.
 */
typedef struct
{
  jack_client_t *client;
  pthread_mutex_t lock;
  int changed;
} jack_watch;

static void watch_note(jack_watch *w)
{
  pthread_mutex_lock(&w->lock);
  w->changed = 1;
  pthread_mutex_unlock(&w->lock);
}

static void watch_on_port(jack_port_id_t port, int registered, void *arg)
{
  (void)port;
  (void)registered;
  watch_note(arg);
}

static void watch_on_client(const char *name, int registered, void *arg)
{
  (void)name;
  (void)registered;
  watch_note(arg);
}

static void *jack_watch_create(void)
{
  jack_watch *w = calloc(1, sizeof(*w));

  if (w == NULL)
  {
    return NULL;
  }

  if (pthread_mutex_init(&w->lock, NULL) != 0)
  {
    free(w);
    return NULL;
  }

  w->client = aud_jack_open_client(JACK_CLIENT_PREFIX "-watch", 1 /* quiet */);
  if (w->client == NULL)
  {
    aud_debug("jack: cannot watch the graph; the list will not update");
    return w; /* a watch that never fires still satisfies the contract */
  }

  jack_set_port_registration_callback(w->client, watch_on_port, w);
  jack_set_client_registration_callback(w->client, watch_on_client, w);

  /* the notifications only arrive once the client is running */
  if (jack_activate(w->client) != 0)
  {
    aud_debug("jack: cannot activate the watch; the list will not update");
    jack_client_close(w->client);
    w->client = NULL;
  }

  return w;
}

static void jack_watch_destroy(void *impl)
{
  jack_watch *w = impl;

  if (w == NULL)
  {
    return;
  }

  if (w->client != NULL)
  {
    jack_client_close(w->client);
  }
  pthread_mutex_destroy(&w->lock);
  free(w);
}

static int jack_watch_changed(void *impl)
{
  jack_watch *w = impl;
  int changed;

  if (w == NULL || w->client == NULL)
  {
    return 0;
  }

  pthread_mutex_lock(&w->lock);
  changed = w->changed;
  w->changed = 0;
  pthread_mutex_unlock(&w->lock);

  return changed;
}

const aud_capture_ops aud_capture_ops_jack = {
    .name = "jack",
    .open_capture = jack_open_capture,
    .close = jack_close,
    .read = jack_read,
    .drop = jack_drop,
    .probe = jack_probe,
    .enumerate = jack_enumerate,
    .watch_create = jack_watch_create,
    .watch_destroy = jack_watch_destroy,
    .watch_changed = jack_watch_changed,
};
