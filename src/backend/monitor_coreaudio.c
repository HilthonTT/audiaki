/* SPDX-License-Identifier: MIT */
/*
 * monitor_coreaudio.c - the CoreAudio playback backend, for monitoring.
 *
 * The capture side's inversion the other way round: the caller pushes frames
 * whenever a period has been captured, and the HAL pulls them when it wants
 * something to play. The same wait-free ring between the two.
 *
 * The output device's rate is left alone, unlike the capture side's. A monitor
 * is a convenience, and moving the rate of whatever the machine is playing
 * through in order to provide one is not a trade worth making - so a take at a
 * rate the output is not at goes through audio/resample.h on the way, exactly
 * as it does for an ALSA output that will not take the capture rate.
 */
#include "audio/resample.h"
#include "backend/backend.h"
#include "backend/coreaudio_common.h"
#include "backend/monitor.h"
#include "util/log.h"
#include "util/ringbuf.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Input frames converted per pass when the output is not at the take's rate. */
#define CA_MONITOR_RESAMPLE_CHUNK 1024u

/*
 * How much to hold. The capture side hands over a period at a time and the HAL
 * asks for its own period at a time; neither is the other's size, so the FIFO
 * needs room for the mismatch without being deep enough to put the monitor
 * audibly behind the strings. About 100 ms at 48 kHz.
 */
#define CA_MONITOR_FIFO_FRAMES 4800u
#define CA_MONITOR_FIFO_PERIODS 4u

/* The output element of an AUHAL. Bus 1 is the input side, which is not used here. */
#define CA_OUTPUT_BUS 0

typedef struct
{
  AudioUnit unit;
  AudioDeviceID device;
  int running;

  unsigned channels;
  unsigned rate;    /* what the output is running at */
  unsigned in_rate; /* what callers hand over, which may not be the same */

  aud_ringbuf fifo; /* interleaved float, `channels` per frame */

  /*
   * Only there when the output is not at the caller's rate. NULL is the
   * ordinary case and the write path costs nothing extra for it.
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
} ca_monitor;

/*
 * The HAL wants a period. Unlike the capture side this one is handed the
 * buffers to fill, so there is nothing to render - only the ring to empty into
 * what it was given.
 */
static OSStatus monitor_render(void *ref, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *stamp, UInt32 bus, UInt32 frames,
                               AudioBufferList *io)
{
  ca_monitor *m = ref;
  float *dst;
  size_t want = (size_t)frames;
  size_t got;

  (void)flags;
  (void)stamp;
  (void)bus;

  if (io == NULL || io->mNumberBuffers == 0)
  {
    return noErr;
  }

  dst = io->mBuffers[0].mData;
  if (dst == NULL)
  {
    return noErr;
  }

  /*
   * The frame count and the buffer's size are two separate claims by the HAL,
   * and this only ever writes as far as the smaller of them. They agree in
   * every ordinary cycle; trusting the count alone is how the one cycle where
   * they do not becomes a write past the end of somebody else's buffer.
   */
  {
    size_t room = io->mBuffers[0].mDataByteSize / (m->channels * sizeof(*dst));

    if (want > room)
    {
      want = room;
    }
  }

  if (m->flushing)
  {
    aud_ringbuf_skip(&m->fifo, aud_ringbuf_available(&m->fifo));
    m->flushing = 0;
  }

  got = aud_ringbuf_read(&m->fifo, dst, want * m->channels) / m->channels;

  if (got < want)
  {
    /*
     * Nothing left to play. Silence rather than a repeat of the last buffer:
     * monitoring is off, or paused, or the capture has not caught up, and a
     * held note would be a worse answer than a gap in any of those.
     */
    memset(dst + got * m->channels, 0, (want - got) * m->channels * sizeof(*dst));
  }

  io->mBuffers[0].mDataByteSize = (UInt32)(want * m->channels * sizeof(*dst));
  return noErr;
}

static void monitor_teardown(ca_monitor *m)
{
  if (m == NULL)
  {
    return;
  }

  /* AudioOutputUnitStop() does not return until the I/O thread is out of the
   * callback, which is what makes the frees below safe */
  if (m->unit != NULL)
  {
    if (m->running)
    {
      AudioOutputUnitStop(m->unit);
      m->running = 0;
    }
    AudioComponentInstanceDispose(m->unit);
    m->unit = NULL;
  }

  aud_ringbuf_free(&m->fifo);
  aud_resample_destroy(m->resampler);
  free(m->converted);
  free(m->scaled);
  free(m);
}

static void ca_monitor_close(void *impl)
{
  monitor_teardown((ca_monitor *)impl);
}

static void *ca_monitor_open(const aud_monitor_config *cfg, unsigned *rate_out,
                             unsigned *channels_out)
{
  const char *name = cfg->name != NULL ? cfg->name : AUD_MONITOR_DEFAULT_DEVICE;
  AudioStreamBasicDescription asbd;
  AURenderCallbackStruct callback;
  ca_monitor *m;
  unsigned available;
  unsigned period;
  size_t fifo_frames;
  char status[16];
  char label[128];
  OSStatus err;

  m = calloc(1, sizeof(*m));
  if (m == NULL)
  {
    aud_warn("monitor: out of memory");
    return NULL;
  }

  m->channels = cfg->channels;
  m->in_rate = cfg->rate;

  m->device = aud_ca_find_device(name, 0 /* output */);
  if (m->device == kAudioObjectUnknown)
  {
    aud_warn("monitor: cannot open playback device '%s'", name);
    free(m);
    return NULL;
  }

  aud_ca_device_name(m->device, label, sizeof(label));

  available = aud_ca_channels(m->device, kAudioObjectPropertyScopeOutput);
  if (available < m->channels)
  {
    aud_warn("monitor: %s does not accept %u channel(s)", label, m->channels);
    free(m);
    return NULL;
  }

  /* the device's own rate, left where whatever else is playing put it */
  m->rate = (unsigned)(aud_ca_rate(m->device) + 0.5);
  if (m->rate == 0)
  {
    aud_warn("monitor: %s did not say what rate it is running at", label);
    free(m);
    return NULL;
  }

  period =
      aud_ca_set_period(m->device, cfg->period_frames > 0 ? cfg->period_frames : 512u);

  m->scaled_cap = CA_MONITOR_RESAMPLE_CHUNK;
  m->scaled = malloc(m->scaled_cap * m->channels * sizeof(*m->scaled));
  if (m->scaled == NULL)
  {
    aud_warn("monitor: out of memory");
    monitor_teardown(m);
    return NULL;
  }

  fifo_frames = CA_MONITOR_FIFO_FRAMES;
  if (fifo_frames < (size_t)period * CA_MONITOR_FIFO_PERIODS)
  {
    fifo_frames = (size_t)period * CA_MONITOR_FIFO_PERIODS;
  }

  if (aud_ringbuf_init(&m->fifo, fifo_frames * m->channels) != 0)
  {
    aud_warn("monitor: out of memory");
    monitor_teardown(m);
    return NULL;
  }

  if (m->rate != m->in_rate)
  {
    aud_info("monitor: %s is running at %u Hz and the audio is %u Hz; converting "
             "on the way out",
             label, m->rate, m->in_rate);

    m->resampler = aud_resample_create(m->in_rate, m->rate, m->channels);
    if (m->resampler != NULL)
    {
      m->converted_cap = aud_resample_out_max(m->resampler, CA_MONITOR_RESAMPLE_CHUNK);
      m->converted = malloc(m->converted_cap * m->channels * sizeof(*m->converted));
    }
    if (m->resampler == NULL || m->converted == NULL)
    {
      /*
       * Without the converter there is nothing sensible to play: the device is
       * running at a rate the audio is not. Better to say so than to play it at
       * the wrong pitch.
       */
      aud_warn("monitor: cannot convert %u Hz to %u Hz, not playing it", m->in_rate,
               m->rate);
      monitor_teardown(m);
      return NULL;
    }
  }

  m->unit = aud_ca_new_unit("playback");
  if (m->unit == NULL)
  {
    monitor_teardown(m);
    return NULL;
  }

  err = AudioUnitSetProperty(m->unit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &m->device, sizeof(m->device));
  if (err != noErr)
  {
    aud_warn("monitor: cannot open playback device '%s': %s", label,
             aud_ca_status_text(err, status, sizeof(status)));
    monitor_teardown(m);
    return NULL;
  }

  /*
   * The format audiaki hands the unit, which is the input scope of the output
   * element. The output scope of that element is the hardware's own.
   */
  asbd = aud_ca_stream_format((double)m->rate, m->channels);
  err = AudioUnitSetProperty(m->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, CA_OUTPUT_BUS, &asbd, sizeof(asbd));
  if (err != noErr)
  {
    aud_warn("monitor: %s will not take %u channel(s) at %u Hz: %s", label, m->channels,
             m->rate, aud_ca_status_text(err, status, sizeof(status)));
    monitor_teardown(m);
    return NULL;
  }

  memset(&callback, 0, sizeof(callback));
  callback.inputProc = monitor_render;
  callback.inputProcRefCon = m;
  err = AudioUnitSetProperty(m->unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, CA_OUTPUT_BUS, &callback,
                             sizeof(callback));
  if (err != noErr)
  {
    aud_warn("monitor: cannot attach the playback callback: %s",
             aud_ca_status_text(err, status, sizeof(status)));
    monitor_teardown(m);
    return NULL;
  }

  err = AudioUnitInitialize(m->unit);
  if (err != noErr)
  {
    aud_warn("monitor: cannot open playback device '%s': %s", label,
             aud_ca_status_text(err, status, sizeof(status)));
    monitor_teardown(m);
    return NULL;
  }

  err = AudioOutputUnitStart(m->unit);
  if (err != noErr)
  {
    aud_warn("monitor: cannot start playback on %s: %s", label,
             aud_ca_status_text(err, status, sizeof(status)));
    monitor_teardown(m);
    return NULL;
  }
  m->running = 1;

  aud_debug("monitor: coreaudio %s, %u Hz, %u ch, device period %u frames, fifo %lu "
            "frames (%.1f ms)",
            label, m->rate, m->channels, period, (unsigned long)fifo_frames,
            1000.0 * (double)fifo_frames / (double)m->rate);

  *rate_out = m->rate;
  *channels_out = m->channels;
  return m;
}

static unsigned long ca_monitor_dropped(const void *impl)
{
  const ca_monitor *m = impl;

  return m != NULL ? m->dropped : 0;
}

/* Frames the FIFO would take right now, in the output's own frames. */
static size_t fifo_space(const ca_monitor *m)
{
  return aud_ringbuf_space(&m->fifo) / m->channels;
}

/*
 * Hand frames already at the output's rate to the FIFO, scaling and clipping on
 * the way. The rate conversion, when there is one, happens in the caller.
 */
static void push_frames(ca_monitor *m, const float *interleaved, size_t frames,
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

static int ca_monitor_write(void *impl, const float *interleaved, size_t frames,
                            float gain)
{
  ca_monitor *m = impl;

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
    size_t take = frames < CA_MONITOR_RESAMPLE_CHUNK ? frames : CA_MONITOR_RESAMPLE_CHUNK;
    size_t got =
        aud_resample_run(m->resampler, interleaved, take, m->converted, m->converted_cap);

    push_frames(m, m->converted, got, gain);

    interleaved += take * m->channels;
    frames -= take;
  }
  return 0;
}

static long ca_monitor_space(void *impl)
{
  ca_monitor *m = impl;
  size_t space;

  if (m == NULL || m->failed)
  {
    return -1;
  }

  space = fifo_space(m);

  /*
   * Answered in the caller's frames rather than the output's. A caller reads
   * its source and hands it over at the source's rate; telling it how much room
   * there is in the output's frames would have it over-read whenever the output
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
#define CA_MONITOR_DRAIN_POLL_MS 5
#define CA_MONITOR_DRAIN_TIMEOUT_MS 2000

/*
 * What to allow the HAL for the frames it has already taken: its own period,
 * plus whatever the device adds after it. A round number rather than a
 * measurement, as the PipeWire monitor's is.
 */
#define CA_MONITOR_DRAIN_TAIL_MS 80

static void sleep_ms(long ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static void ca_monitor_drain(void *impl)
{
  ca_monitor *m = impl;
  long waited = 0;

  if (m == NULL || m->failed)
  {
    return;
  }

  while (aud_ringbuf_available(&m->fifo) > 0 && !m->failed &&
         waited < CA_MONITOR_DRAIN_TIMEOUT_MS)
  {
    sleep_ms(CA_MONITOR_DRAIN_POLL_MS);
    waited += CA_MONITOR_DRAIN_POLL_MS;
  }

  /* an empty FIFO means the HAL has taken everything, not that it has played it */
  sleep_ms(CA_MONITOR_DRAIN_TAIL_MS);
}

static void ca_monitor_flush(void *impl)
{
  ca_monitor *m = impl;

  if (m == NULL)
  {
    return;
  }

  /*
   * Asked of the callback rather than done here: it is the FIFO's only reader,
   * and that is what makes the ring safe without a lock. One period's worth of
   * audio - a millisecond or two - is heard before it takes effect, against the
   * tenth of a second the FIFO holds.
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

const aud_monitor_ops aud_monitor_ops_coreaudio = {
    .name = "coreaudio",
    .open = ca_monitor_open,
    .close = ca_monitor_close,
    .write = ca_monitor_write,
    .dropped = ca_monitor_dropped,
    .space = ca_monitor_space,
    .drain = ca_monitor_drain,
    .flush = ca_monitor_flush,
};
