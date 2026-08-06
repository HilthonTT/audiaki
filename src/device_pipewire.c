/* SPDX-License-Identifier: MIT */
/*
 * device_pipewire.c - the PipeWire capture backend.
 *
 * Asks the sound server that owns the card rather than opening the card. That
 * buys three things ALSA cannot give: coexisting with whatever else has the
 * interface open, device names that match the rest of the desktop, and
 * capturing the output of another application through a sink's monitor.
 *
 * PipeWire pushes - it calls process() when a buffer is ready - and audiaki
 * pulls, because aud_device_read() is a blocking read that recorder.c and
 * tune.c drive their own loops from. The inversion is absorbed here, in a byte
 * FIFO between the loop thread and the caller, so nothing above device.h has to
 * know that this backend's frames arrive rather than being fetched.
 *
 * The FIFO holds bytes in the negotiated capture format rather than floats, so
 * the samples reaching the WAV writer are the ones the server sent, unrounded.
 */
#include "backend.h"
#include "device.h"
#include "jsonout.h"
#include "log.h"
#include "version.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Seconds to wait for the server to answer a round trip before giving up. */
#define PW_ROUNDTRIP_TIMEOUT 2

/* Seconds aud_device_read() blocks before returning "nothing yet, ask again". */
#define PW_READ_TIMEOUT_NS 200000000L

/*
 * How much capture to hold when the caller is not reading. Sized from the
 * period the caller asked for, with a floor: a FIFO shorter than the server's
 * quantum would drop part of every buffer that arrives.
 */
#define PW_FIFO_MIN_FRAMES 8192u

static pthread_once_t pw_init_once = PTHREAD_ONCE_INIT;

static void pw_init_call(void)
{
  pw_init(NULL, NULL);
}

static void ensure_pw_init(void)
{
  pthread_once(&pw_init_once, pw_init_call);
}

/* -- a connection to the server ------------------------------------------- */

typedef struct
{
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct spa_hook core_listener;
  int pending_seq;
  int done;
  int failed;
} pw_conn;

static void on_core_done(void *data, uint32_t id, int seq)
{
  pw_conn *c = data;

  if (id == PW_ID_CORE && seq == c->pending_seq)
  {
    c->done = 1;
    pw_thread_loop_signal(c->loop, false);
  }
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  pw_conn *c = data;

  (void)seq;
  aud_debug("pipewire: error on object %u: %s (%s)", id, message, spa_strerror(res));
  if (id == PW_ID_CORE)
  {
    c->failed = 1;
    pw_thread_loop_signal(c->loop, false);
  }
}

static const struct pw_core_events conn_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

static void conn_close(pw_conn *c)
{
  if (c->loop != NULL)
  {
    pw_thread_loop_stop(c->loop);
  }
  if (c->core != NULL)
  {
    spa_hook_remove(&c->core_listener);
    pw_core_disconnect(c->core);
    c->core = NULL;
  }
  if (c->context != NULL)
  {
    pw_context_destroy(c->context);
    c->context = NULL;
  }
  if (c->loop != NULL)
  {
    pw_thread_loop_destroy(c->loop);
    c->loop = NULL;
  }
}

/*
 * Bring up a loop, a context and a connected core. Returns 0 with the loop
 * running and unlocked, or -1 having cleaned up whatever it managed to build.
 * `quiet` suppresses the diagnostic, for the probe that only wants a yes or no.
 */
static int conn_open(pw_conn *c, const char *name, int quiet)
{
  memset(c, 0, sizeof(*c));

  ensure_pw_init();

  c->loop = pw_thread_loop_new(name, NULL);
  if (c->loop == NULL)
  {
    if (!quiet)
    {
      aud_error("pipewire: cannot create the event loop");
    }
    return -1;
  }

  c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
  if (c->context == NULL)
  {
    if (!quiet)
    {
      aud_error("pipewire: cannot create the context");
    }
    conn_close(c);
    return -1;
  }

  if (pw_thread_loop_start(c->loop) != 0)
  {
    if (!quiet)
    {
      aud_error("pipewire: cannot start the event loop");
    }
    conn_close(c);
    return -1;
  }

  pw_thread_loop_lock(c->loop);
  c->core = pw_context_connect(c->context, NULL, 0);
  if (c->core == NULL)
  {
    pw_thread_loop_unlock(c->loop);
    if (!quiet)
    {
      aud_error("pipewire: cannot connect to the server: %s", strerror(errno));
      aud_info("is the daemon running? try --backend alsa to open the card directly");
    }
    conn_close(c);
    return -1;
  }
  pw_core_add_listener(c->core, &c->core_listener, &conn_core_events, c);
  pw_thread_loop_unlock(c->loop);

  return 0;
}

/*
 * Wait until the server has finished everything asked of it so far. The loop
 * must be locked; it stays locked. Returns 0, or -1 on error or timeout.
 */
static int conn_roundtrip(pw_conn *c)
{
  c->done = 0;
  c->failed = 0;
  c->pending_seq = pw_core_sync(c->core, PW_ID_CORE, 0);

  while (!c->done && !c->failed)
  {
    if (pw_thread_loop_timed_wait(c->loop, PW_ROUNDTRIP_TIMEOUT) != 0)
    {
      aud_debug("pipewire: the server did not answer within %d s", PW_ROUNDTRIP_TIMEOUT);
      return -1;
    }
  }

  return c->failed ? -1 : 0;
}

int aud_pipewire_daemon_responds(void)
{
  static int cached = -1; /* -1 unknown, 0 no, 1 yes */
  pw_conn c;
  int ok;

  if (cached >= 0)
  {
    return cached;
  }

  if (conn_open(&c, "audiaki-probe", 1 /* quiet */) != 0)
  {
    cached = 0;
    return 0;
  }

  pw_thread_loop_lock(c.loop);
  ok = conn_roundtrip(&c) == 0;
  pw_thread_loop_unlock(c.loop);
  conn_close(&c);

  cached = ok;
  return cached;
}

/* -- the nodes the server is offering -------------------------------------- */

/*
 * A node worth capturing from. Sinks are in here as well as sources: a sink's
 * monitor is how you record what another application is playing, which is the
 * one thing this backend can do that opening the card never could.
 */
typedef struct
{
  char name[64];        /* node.name - what -D takes */
  char card[80];        /* node.description, or the name again */
  char description[80]; /* what it is, in words */
  int is_sink;          /* capture its monitor rather than its input */
} pw_node;

typedef struct
{
  pw_conn conn;
  struct pw_registry *registry;
  struct spa_hook registry_listener;
  pw_node *nodes;
  int count;
  int oom;
  /* set by the watch: something appeared or disappeared since the last ask */
  int changed;
} pw_nodes;

/* Whether a media.class is something audiaki can take audio from. */
static int class_is_capturable(const char *media_class, int *is_sink)
{
  if (media_class == NULL)
  {
    return 0;
  }

  if (strcmp(media_class, "Audio/Source") == 0 ||
      strcmp(media_class, "Audio/Source/Virtual") == 0 ||
      strcmp(media_class, "Audio/Duplex") == 0)
  {
    *is_sink = 0;
    return 1;
  }

  /* a sink is captured through its monitor */
  if (strcmp(media_class, "Audio/Sink") == 0)
  {
    *is_sink = 1;
    return 1;
  }

  return 0;
}

static void nodes_add(pw_nodes *n, const struct spa_dict *props)
{
  const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  const char *nick = spa_dict_lookup(props, PW_KEY_NODE_NICK);
  pw_node *grown;
  int is_sink = 0;

  if (node_name == NULL || !class_is_capturable(media_class, &is_sink))
  {
    return;
  }

  grown = realloc(n->nodes, (size_t)(n->count + 1) * sizeof(*n->nodes));
  if (grown == NULL)
  {
    n->oom = 1;
    return;
  }
  n->nodes = grown;

  memset(&grown[n->count], 0, sizeof(grown[n->count]));
  snprintf(grown[n->count].name, sizeof(grown[n->count].name), "%s", node_name);
  snprintf(grown[n->count].card, sizeof(grown[n->count].card), "%s",
           desc != NULL ? desc : (nick != NULL ? nick : node_name));
  snprintf(grown[n->count].description, sizeof(grown[n->count].description), "%s",
           is_sink ? "monitor of this output" : media_class);
  grown[n->count].is_sink = is_sink;
  n->count++;
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props)
{
  pw_nodes *n = data;
  int is_sink = 0;

  (void)id;
  (void)permissions;
  (void)version;

  if (type == NULL || strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == NULL)
  {
    return;
  }

  if (n->nodes == NULL && n->count == 0 && n->changed == 0)
  {
    /* first pass: collecting */
  }

  if (class_is_capturable(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS), &is_sink))
  {
    n->changed = 1;
  }
  nodes_add(n, props);
}

static void on_registry_global_remove(void *data, uint32_t id)
{
  pw_nodes *n = data;

  (void)id;
  /*
   * The id alone does not say whether what went was a node audiaki cares
   * about, and binding every global to find out would cost more than the
   * re-walk this schedules. A spurious rebuild is a list that comes back the
   * same.
   */
  n->changed = 1;
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void nodes_free(pw_nodes *n)
{
  /*
   * Stop the loop before touching the proxy. A proxy belongs to the thread
   * running the loop, and destroying one from here while that thread is live is
   * exactly what the server means by "called from wrong context". Stopping is
   * idempotent, so conn_close() doing it again below is harmless.
   */
  if (n->conn.loop != NULL)
  {
    pw_thread_loop_stop(n->conn.loop);
  }

  if (n->registry != NULL)
  {
    spa_hook_remove(&n->registry_listener);
    pw_proxy_destroy((struct pw_proxy *)n->registry);
    n->registry = NULL;
  }
  conn_close(&n->conn);
  free(n->nodes);
  n->nodes = NULL;
  n->count = 0;
}

/*
 * Connect, walk the registry once, and disconnect. Returns 0 with the nodes in
 * `n->nodes`, which the caller releases with nodes_free().
 */
static int nodes_collect(pw_nodes *n, int quiet)
{
  memset(n, 0, sizeof(*n));

  if (conn_open(&n->conn, "audiaki-enum", quiet) != 0)
  {
    return -1;
  }

  pw_thread_loop_lock(n->conn.loop);
  n->registry = pw_core_get_registry(n->conn.core, PW_VERSION_REGISTRY, 0);
  if (n->registry == NULL)
  {
    pw_thread_loop_unlock(n->conn.loop);
    if (!quiet)
    {
      aud_error("pipewire: cannot read the registry");
    }
    nodes_free(n);
    return -1;
  }
  pw_registry_add_listener(n->registry, &n->registry_listener, &registry_events, n);

  /*
   * One round trip is enough: the server sends every existing global before it
   * answers the sync, so by the time this returns the list is complete.
   */
  if (conn_roundtrip(&n->conn) != 0)
  {
    pw_thread_loop_unlock(n->conn.loop);
    if (!quiet)
    {
      aud_error("pipewire: the server did not finish listing its devices");
    }
    nodes_free(n);
    return -1;
  }
  pw_thread_loop_unlock(n->conn.loop);

  if (n->oom)
  {
    aud_error("out of memory listing devices");
    nodes_free(n);
    return -1;
  }

  return 0;
}

static int pipewire_enumerate(aud_device_entry **out)
{
  pw_nodes n;
  aud_device_entry *list;

  if (out == NULL)
  {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;

  if (nodes_collect(&n, 0) != 0)
  {
    return -1;
  }

  if (n.count == 0)
  {
    nodes_free(&n);
    return 0;
  }

  list = calloc((size_t)n.count, sizeof(*list));
  if (list == NULL)
  {
    aud_error("out of memory listing devices");
    nodes_free(&n);
    return -1;
  }

  for (int i = 0; i < n.count; i++)
  {
    snprintf(list[i].name, sizeof(list[i].name), "%s", n.nodes[i].name);
    snprintf(list[i].card, sizeof(list[i].card), "%s", n.nodes[i].card);
    snprintf(list[i].description, sizeof(list[i].description), "%s",
             n.nodes[i].description);
  }

  {
    int found = n.count;

    nodes_free(&n);
    *out = list;
    return found;
  }
}

/* -- probe ----------------------------------------------------------------- */

/*
 * What a device supports, through a server that converts. The honest answer is
 * different in kind from ALSA's: the hardware's own format list is not what
 * governs, because anything audiaki asks for is provided by conversion. So this
 * reports what the stream will actually be given, and says where that comes
 * from, rather than printing a hardware capability table that no longer decides
 * anything.
 */
static int pipewire_probe(const char *name, int json)
{
  pw_nodes n;
  const pw_node *match = NULL;

  if (nodes_collect(&n, 0) != 0)
  {
    return -1;
  }

  if (name != NULL && strcmp(name, AUD_DEFAULT_DEVICE) != 0)
  {
    for (int i = 0; i < n.count; i++)
    {
      if (strcmp(n.nodes[i].name, name) == 0)
      {
        match = &n.nodes[i];
        break;
      }
    }

    if (match == NULL)
    {
      aud_error("no such capture device '%s'", name);
      aud_info("run '" AUDIAKI_NAME " --list' to see what the server is offering");
      nodes_free(&n);
      return -1;
    }
  }

  if (json)
  {
    fputs("{\n  \"device\": ", stdout);
    aud_json_string(stdout, name != NULL ? name : AUD_DEFAULT_DEVICE);
    fputs(",\n  \"backend\": \"pipewire\"", stdout);
    fputs(",\n  \"description\": ", stdout);
    aud_json_string(stdout, match != NULL ? match->card : "the server's default source");
    printf(",\n  \"monitor\": %s", (match != NULL && match->is_sink) ? "true" : "false");
    fputs(",\n  \"formats\": [\"S16_LE\", \"S24_3LE\", \"S24_LE\", \"S32_LE\"]", stdout);
    printf(",\n  \"channels\": {\"min\": %u, \"max\": %u}", 1u, SPA_AUDIO_MAX_CHANNELS);
    printf(",\n  \"rates\": {\"min\": %u, \"max\": %u}", 8000u, 768000u);
    fputs(",\n  \"converted\": true\n}\n", stdout);
  }
  else
  {
    printf("device:   %s\n", name != NULL ? name : AUD_DEFAULT_DEVICE);
    printf("backend:  pipewire\n");
    printf("what:     %s\n", match != NULL ? match->card : "the server's default source");
    if (match != NULL && match->is_sink)
    {
      printf("monitor:  yes - this captures what is being played to this output\n");
    }
    printf("formats:  S16_LE S24_3LE S24_LE S32_LE\n");
    printf("channels: 1..%u\n", SPA_AUDIO_MAX_CHANNELS);
    printf("rates:    8000..768000 Hz\n");
    printf("\n");
    printf("These are what the stream will be given, not what the hardware does\n");
    printf("natively: PipeWire converts. Use --backend alsa to ask the card.\n");
  }

  nodes_free(&n);
  return 0;
}

/* -- the capture stream ---------------------------------------------------- */

typedef struct
{
  pw_conn conn;
  struct pw_stream *stream;
  struct spa_hook stream_listener;

  unsigned rate;
  unsigned channels;
  aud_format format;
  size_t frame_bytes;

  /* the FIFO between the loop thread and aud_device_read() */
  pthread_mutex_t lock;
  pthread_cond_t cond;
  uint8_t *fifo;
  size_t fifo_bytes; /* allocation */
  size_t head;       /* read offset */
  size_t fill;       /* bytes held */
  unsigned overruns; /* times a buffer did not fit */
  int negotiated;
  int broken;
} pw_capture;

static enum spa_audio_format to_spa_format(aud_format fmt)
{
  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    return SPA_AUDIO_FORMAT_S16_LE;
  case AUD_FORMAT_S24_3LE:
    return SPA_AUDIO_FORMAT_S24_LE; /* SPA's S24 is the packed three-byte one */
  case AUD_FORMAT_S24_LE:
    return SPA_AUDIO_FORMAT_S24_32_LE;
  case AUD_FORMAT_S32_LE:
    return SPA_AUDIO_FORMAT_S32_LE;
  case AUD_FORMAT_UNKNOWN:
  default:
    return SPA_AUDIO_FORMAT_UNKNOWN;
  }
}

static aud_format from_spa_format(enum spa_audio_format fmt)
{
  switch (fmt)
  {
  case SPA_AUDIO_FORMAT_S16_LE:
    return AUD_FORMAT_S16_LE;
  case SPA_AUDIO_FORMAT_S24_LE:
    return AUD_FORMAT_S24_3LE;
  case SPA_AUDIO_FORMAT_S24_32_LE:
    return AUD_FORMAT_S24_LE;
  case SPA_AUDIO_FORMAT_S32_LE:
    return AUD_FORMAT_S32_LE;
  default:
    return AUD_FORMAT_UNKNOWN;
  }
}

/* Producer side: append to the FIFO, dropping the arrival if it will not fit. */
static void fifo_push(pw_capture *c, const uint8_t *src, size_t bytes)
{
  size_t tail;
  size_t first;

  if (bytes == 0)
  {
    return;
  }

  pthread_mutex_lock(&c->lock);

  if (bytes > c->fifo_bytes - c->fill)
  {
    /*
     * Nobody is reading fast enough. Dropping what just arrived rather than
     * the backlog keeps the FIFO's contents contiguous in time, and this is
     * counted as an xrun because that is exactly what it is.
     */
    c->overruns++;
    pthread_mutex_unlock(&c->lock);
    return;
  }

  tail = (c->head + c->fill) % c->fifo_bytes;
  first = c->fifo_bytes - tail;
  if (first > bytes)
  {
    first = bytes;
  }

  memcpy(c->fifo + tail, src, first);
  if (bytes > first)
  {
    memcpy(c->fifo, src + first, bytes - first);
  }
  c->fill += bytes;

  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->lock);
}

static void on_stream_process(void *userdata)
{
  pw_capture *c = userdata;
  struct pw_buffer *b;
  struct spa_data *d;
  uint32_t offset;
  uint32_t size;

  b = pw_stream_dequeue_buffer(c->stream);
  if (b == NULL)
  {
    return;
  }

  d = &b->buffer->datas[0];
  if (d->data != NULL && d->chunk != NULL)
  {
    offset = SPA_MIN(d->chunk->offset, d->maxsize);
    size = SPA_MIN(d->chunk->size, d->maxsize - offset);
    fifo_push(c, (const uint8_t *)d->data + offset, size);
  }

  pw_stream_queue_buffer(c->stream, b);
}

static void on_stream_param_changed(void *userdata, uint32_t id,
                                    const struct spa_pod *param)
{
  pw_capture *c = userdata;
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

  pthread_mutex_lock(&c->lock);
  c->rate = info.info.raw.rate;
  c->channels = info.info.raw.channels;
  c->format = from_spa_format(info.info.raw.format);
  c->frame_bytes = (size_t)aud_format_hw_bytes(c->format) * c->channels;
  c->negotiated = 1;
  pthread_mutex_unlock(&c->lock);

  pw_thread_loop_signal(c->conn.loop, false);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
  pw_capture *c = userdata;

  (void)old;

  if (state == PW_STREAM_STATE_ERROR)
  {
    aud_error("pipewire: capture stream failed: %s", error != NULL ? error : "unknown");
    pthread_mutex_lock(&c->lock);
    c->broken = 1;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->lock);
    pw_thread_loop_signal(c->conn.loop, false);
  }
  else if (state == PW_STREAM_STATE_UNCONNECTED)
  {
    /*
     * The node went away - the interface was unplugged, or the server moved
     * the stream and could not. Waking the reader turns this into the same
     * end-of-stream the ALSA backend reports, so a take stops with what was
     * written rather than hanging on a device that is gone.
     */
    pthread_mutex_lock(&c->lock);
    c->broken = 1;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->lock);
    pw_thread_loop_signal(c->conn.loop, false);
  }
}

static const struct pw_stream_events capture_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

static void capture_free(pw_capture *c)
{
  if (c == NULL)
  {
    return;
  }

  /* stop first, for the same reason nodes_free() does: the stream is the loop
   * thread's, and tearing it down under that thread is a race */
  if (c->conn.loop != NULL)
  {
    pw_thread_loop_stop(c->conn.loop);
  }

  if (c->stream != NULL)
  {
    spa_hook_remove(&c->stream_listener);
    pw_stream_destroy(c->stream);
    c->stream = NULL;
  }

  conn_close(&c->conn);
  pthread_cond_destroy(&c->cond);
  pthread_mutex_destroy(&c->lock);
  free(c->fifo);
  free(c);
}

static void pipewire_close(aud_device *dev)
{
  if (dev->handle == NULL)
  {
    return;
  }

  capture_free((pw_capture *)dev->handle);
  dev->handle = NULL;
}

/* Whether `name` is a sink, whose monitor is what we would be capturing. */
static int name_is_sink(const char *name)
{
  pw_nodes n;
  int is_sink = 0;

  if (name == NULL || strcmp(name, AUD_DEFAULT_DEVICE) == 0)
  {
    return 0;
  }

  if (nodes_collect(&n, 1 /* quiet */) != 0)
  {
    return 0;
  }

  for (int i = 0; i < n.count; i++)
  {
    if (strcmp(n.nodes[i].name, name) == 0)
    {
      is_sink = n.nodes[i].is_sink;
      break;
    }
  }

  nodes_free(&n);
  return is_sink;
}

static int pipewire_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  uint8_t builder_buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(builder_buffer, sizeof(builder_buffer));
  const struct spa_pod *params[1];
  struct spa_audio_info_raw raw;
  struct pw_properties *props;
  aud_format wanted;
  pw_capture *c;
  unsigned fifo_frames;
  char latency[64];
  int is_sink;

  /*
   * PipeWire converts, so there is nothing to negotiate down through: asking
   * for the widest gets the widest whatever the hardware is. That is a real
   * difference from ALSA, where the candidate list exists because the card can
   * refuse - here the only refusal would be the server declining to convert,
   * which it does not do for PCM.
   */
  wanted = cfg->format != AUD_FORMAT_UNKNOWN ? cfg->format : AUD_FORMAT_S32_LE;

  c = calloc(1, sizeof(*c));
  if (c == NULL)
  {
    aud_error("out of memory opening the capture stream");
    return -1;
  }

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

  c->rate = cfg->rate;
  c->channels = cfg->channels;
  c->format = wanted;
  c->frame_bytes = (size_t)aud_format_hw_bytes(wanted) * cfg->channels;

  fifo_frames = cfg->period_frames * cfg->periods;
  if (fifo_frames < PW_FIFO_MIN_FRAMES)
  {
    fifo_frames = PW_FIFO_MIN_FRAMES;
  }
  c->fifo_bytes = (size_t)fifo_frames * c->frame_bytes;
  c->fifo = malloc(c->fifo_bytes);
  if (c->fifo == NULL)
  {
    aud_error("out of memory sizing the capture buffer");
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->lock);
    free(c);
    return -1;
  }

  if (conn_open(&c->conn, "audiaki-capture", 0) != 0)
  {
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->lock);
    free(c->fifo);
    free(c);
    return -1;
  }

  is_sink = name_is_sink(cfg->name);

  snprintf(latency, sizeof(latency), "%u/%u", cfg->period_frames, cfg->rate);

  props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                            PW_KEY_MEDIA_ROLE, "Production", PW_KEY_APP_NAME,
                            AUDIAKI_NAME, PW_KEY_NODE_NAME, AUDIAKI_NAME,
                            PW_KEY_NODE_LATENCY, latency, NULL);
  if (props == NULL)
  {
    aud_error("out of memory describing the capture stream");
    capture_free(c);
    return -1;
  }

  /* "default" means whatever the server considers the current source */
  if (cfg->name != NULL && strcmp(cfg->name, AUD_DEFAULT_DEVICE) != 0)
  {
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg->name);
  }
  if (is_sink)
  {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    aud_debug("pipewire: capturing the monitor of %s", cfg->name);
  }

  pw_thread_loop_lock(c->conn.loop);

  c->stream = pw_stream_new(c->conn.core, AUDIAKI_NAME " capture", props);
  if (c->stream == NULL)
  {
    pw_thread_loop_unlock(c->conn.loop);
    aud_error("pipewire: cannot create the capture stream");
    capture_free(c);
    return -1;
  }
  pw_stream_add_listener(c->stream, &c->stream_listener, &capture_stream_events, c);

  spa_zero(raw);
  raw.format = to_spa_format(wanted);
  raw.rate = cfg->rate;
  raw.channels = cfg->channels;
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);

  if (pw_stream_connect(c->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params,
                        1) < 0)
  {
    pw_thread_loop_unlock(c->conn.loop);
    aud_error("pipewire: cannot connect the capture stream to '%s'", cfg->name);
    aud_info("run '" AUDIAKI_NAME " --list' to see what the server is offering");
    capture_free(c);
    return -1;
  }

  /*
   * Wait for the format to come back before returning: everything above this
   * sizes its buffers from dev->format and dev->channels, and handing those
   * out before the server has agreed them would mean a WAV header describing a
   * stream that never arrived.
   */
  while (!c->negotiated && !c->broken)
  {
    if (pw_thread_loop_timed_wait(c->conn.loop, PW_ROUNDTRIP_TIMEOUT) != 0)
    {
      pw_thread_loop_unlock(c->conn.loop);
      aud_error("pipewire: '%s' did not agree a format within %d s", cfg->name,
                PW_ROUNDTRIP_TIMEOUT);
      capture_free(c);
      return -1;
    }
  }

  pw_thread_loop_unlock(c->conn.loop);

  if (c->broken)
  {
    aud_error("pipewire: cannot capture from '%s'", cfg->name);
    capture_free(c);
    return -1;
  }

  if (c->format == AUD_FORMAT_UNKNOWN)
  {
    aud_error("pipewire: the server agreed a sample format audiaki cannot write");
    capture_free(c);
    return -1;
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
    aud_warn("requested %u Hz, server negotiated %u Hz", cfg->rate, c->rate);
  }

  aud_debug("opened %s through pipewire: %s, %u Hz, %u ch, period %lu frames, "
            "fifo %lu frames",
            dev->name, aud_format_name(dev->format), dev->rate, dev->channels,
            dev->period_frames, dev->buffer_frames);
  return 0;
}

static long pipewire_read(aud_device *dev, void *buf, unsigned long frames,
                          unsigned *xruns)
{
  pw_capture *c = (pw_capture *)dev->handle;
  size_t wanted = (size_t)frames * c->frame_bytes;
  size_t got;
  size_t first;
  unsigned overruns;

  pthread_mutex_lock(&c->lock);

  while (c->fill == 0 && !c->broken)
  {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += PW_READ_TIMEOUT_NS;
    if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_sec += deadline.tv_nsec / 1000000000L;
      deadline.tv_nsec %= 1000000000L;
    }

    if (pthread_cond_timedwait(&c->cond, &c->lock, &deadline) == ETIMEDOUT)
    {
      /*
       * Nothing arrived. Returning "retry" rather than waiting forever is what
       * lets the caller's loop notice Ctrl+C on a silent or stalled device.
       */
      pthread_mutex_unlock(&c->lock);
      return 0;
    }
  }

  if (c->broken && c->fill == 0)
  {
    pthread_mutex_unlock(&c->lock);
    aud_error("capture stopped: the device is no longer available");
    return -1;
  }

  got = c->fill < wanted ? c->fill : wanted;
  got -= got % c->frame_bytes; /* whole frames only */

  first = c->fifo_bytes - c->head;
  if (first > got)
  {
    first = got;
  }
  memcpy(buf, c->fifo + c->head, first);
  if (got > first)
  {
    memcpy((uint8_t *)buf + first, c->fifo, got - first);
  }

  c->head = (c->head + got) % c->fifo_bytes;
  c->fill -= got;

  overruns = c->overruns;
  c->overruns = 0;

  pthread_mutex_unlock(&c->lock);

  if (overruns > 0 && xruns != NULL)
  {
    *xruns += overruns;
  }

  return (long)(got / c->frame_bytes);
}

static void pipewire_drop(aud_device *dev)
{
  pw_capture *c = (pw_capture *)dev->handle;

  if (c == NULL)
  {
    return;
  }

  pw_thread_loop_lock(c->conn.loop);
  if (c->stream != NULL)
  {
    pw_stream_set_active(c->stream, false);
  }
  pw_thread_loop_unlock(c->conn.loop);

  pthread_mutex_lock(&c->lock);
  c->head = 0;
  c->fill = 0;
  pthread_mutex_unlock(&c->lock);
}

/* -- the hotplug watch ----------------------------------------------------- */

/*
 * PipeWire says when something appears or goes, so unlike the ALSA watch there
 * is nothing to poll and no settling delay to guess at: the registry event is
 * the answer, and it arrives when the server has finished with the object.
 */
typedef struct
{
  pw_nodes nodes; /* held open, unlike the one-shot enumeration */
  int started;
} pw_watch;

static void *pipewire_watch_create(void)
{
  pw_watch *w = calloc(1, sizeof(*w));

  if (w == NULL)
  {
    return NULL;
  }

  memset(&w->nodes, 0, sizeof(w->nodes));

  if (conn_open(&w->nodes.conn, "audiaki-watch", 1 /* quiet */) != 0)
  {
    aud_debug("pipewire: cannot watch for devices; the list will not update");
    return w; /* a watch that never fires still satisfies the contract */
  }

  pw_thread_loop_lock(w->nodes.conn.loop);
  w->nodes.registry = pw_core_get_registry(w->nodes.conn.core, PW_VERSION_REGISTRY, 0);
  if (w->nodes.registry != NULL)
  {
    pw_registry_add_listener(w->nodes.registry, &w->nodes.registry_listener,
                             &registry_events, &w->nodes);
    w->started = 1;
  }
  pw_thread_loop_unlock(w->nodes.conn.loop);

  /*
   * The globals that already exist arrive as events too, so the first ask would
   * otherwise always say "changed" for a list the caller has just built.
   */
  if (w->started)
  {
    pw_thread_loop_lock(w->nodes.conn.loop);
    conn_roundtrip(&w->nodes.conn);
    w->nodes.changed = 0;
    pw_thread_loop_unlock(w->nodes.conn.loop);
  }

  return w;
}

static void pipewire_watch_destroy(void *impl)
{
  pw_watch *w = impl;

  if (w == NULL)
  {
    return;
  }

  nodes_free(&w->nodes);
  free(w);
}

static int pipewire_watch_changed(void *impl)
{
  pw_watch *w = impl;
  int changed;

  if (w == NULL || !w->started)
  {
    return 0;
  }

  pw_thread_loop_lock(w->nodes.conn.loop);
  changed = w->nodes.changed;
  w->nodes.changed = 0;
  pw_thread_loop_unlock(w->nodes.conn.loop);

  return changed;
}

const aud_capture_ops aud_capture_ops_pipewire = {
    .name = "pipewire",
    .open_capture = pipewire_open_capture,
    .close = pipewire_close,
    .read = pipewire_read,
    .drop = pipewire_drop,
    .probe = pipewire_probe,
    .enumerate = pipewire_enumerate,
    .watch_create = pipewire_watch_create,
    .watch_destroy = pipewire_watch_destroy,
    .watch_changed = pipewire_watch_changed,
};
