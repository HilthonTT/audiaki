/* SPDX-License-Identifier: MIT */
/*
 * device_coreaudio.c - the CoreAudio capture backend, for macOS.
 *
 * The same job device_alsa.c does on Linux: open the machine's own audio
 * system, with no sound server in between, because on macOS there is nothing in
 * between - the HAL is both the driver layer and the mixer, and every
 * application on the box is already sharing devices through it. So this backend
 * gets ALSA's directness and PipeWire's coexistence at once, and there is
 * nothing to fall back to when it is missing, because it cannot be.
 *
 * CoreAudio pushes and audiaki pulls, so the inversion is absorbed the way the
 * other two callback backends absorb it: a wait-free ring between the HAL's
 * I/O thread and aud_device_read(), with a condition variable the reader sleeps
 * on and the callback only ever signals without waiting. The ring holds float,
 * which is what the HAL hands over, and the encoding into the capture format
 * happens in the reader.
 *
 * Two things here are the HAL's rather than audiaki's, and both are visible to
 * the user. A device's rate is a property of the device and not of this stream,
 * so asking for one moves it for everything else using that device. And a
 * device is named by its UID, which is ugly and is the only name that is both
 * unique and the same tomorrow.
 */
#include "backend/backend.h"
#include "backend/coreaudio_common.h"
#include "backend/device.h"
#include "util/jsonout.h"
#include "util/log.h"
#include "util/ringbuf.h"
#include "version.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Nanoseconds aud_device_read() blocks before returning "nothing yet, ask again". */
#define CA_READ_TIMEOUT_NS 200000000L

/* How much capture to hold when the caller is not reading; see device_jack.c. */
#define CA_FIFO_MIN_FRAMES 8192u
#define CA_FIFO_MIN_PERIODS 4u

/* The input element of an AUHAL. Bus 0 is the output side, which is not used here. */
#define CA_INPUT_BUS 1
#define CA_OUTPUT_BUS 0

/* -- listing --------------------------------------------------------------- */

static int coreaudio_enumerate(aud_device_entry **out)
{
  AudioDeviceID *devices = NULL;
  AudioDeviceID fallback;
  aud_device_entry *list = NULL;
  unsigned count;
  int found = 0;

  if (out == NULL)
  {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;

  count = aud_ca_devices(&devices);
  if (count == 0)
  {
    /*
     * No devices is an answer, not a failure. Callers that watch for hardware
     * being plugged in ask again and again, and every one of those asks would
     * otherwise be an error on a machine with nothing attached yet.
     */
    return 0;
  }

  fallback = aud_ca_default_device(1 /* input */);

  for (unsigned i = 0; i < count; i++)
  {
    unsigned channels = aud_ca_channels(devices[i], kAudioObjectPropertyScopeInput);
    aud_device_entry *grown;
    char uid[128];
    char label[128];

    if (channels == 0)
    {
      continue; /* an output-only device is not a capture device */
    }

    aud_ca_device_uid(devices[i], uid, sizeof(uid));
    aud_ca_device_name(devices[i], label, sizeof(label));
    if (uid[0] == '\0')
    {
      continue; /* nothing to put in -D, so nothing worth listing */
    }

    grown = realloc(list, (size_t)(found + 1) * sizeof(*list));
    if (grown == NULL)
    {
      aud_error("out of memory listing devices");
      free(devices);
      free(list);
      return -1;
    }
    list = grown;

    memset(&list[found], 0, sizeof(list[found]));
    snprintf(list[found].name, sizeof(list[found].name), "%s", uid);
    snprintf(list[found].card, sizeof(list[found].card), "%s", label);
    snprintf(list[found].description, sizeof(list[found].description),
             "%u channel(s), %.0f Hz%s", channels, aud_ca_rate(devices[i]),
             devices[i] == fallback ? ", the system default" : "");
    found++;
  }

  free(devices);
  *out = list;
  return found;
}

static int coreaudio_probe(const char *name, int json)
{
  AudioDeviceID device = aud_ca_find_device(name, 1 /* input */);
  AudioValueRange *rates = NULL;
  AudioValueRange period_range;
  unsigned channels;
  UInt32 bytes;
  unsigned rate_count;
  char uid[128];
  char label[128];

  if (device == kAudioObjectUnknown)
  {
    aud_error("no such capture device '%s'", name != NULL ? name : AUD_DEFAULT_DEVICE);
    aud_info("run '" AUDIAKI_NAME " --list' to see the available capture devices");
    return -1;
  }

  channels = aud_ca_channels(device, kAudioObjectPropertyScopeInput);
  aud_ca_device_uid(device, uid, sizeof(uid));
  aud_ca_device_name(device, label, sizeof(label));

  bytes = aud_ca_get_alloc(device, kAudioDevicePropertyAvailableNominalSampleRates,
                           kAudioObjectPropertyScopeGlobal, (void **)&rates);
  rate_count = (unsigned)(bytes / sizeof(AudioValueRange));

  memset(&period_range, 0, sizeof(period_range));
  aud_ca_get(device, kAudioDevicePropertyBufferFrameSizeRange,
             kAudioObjectPropertyScopeGlobal, sizeof(period_range), &period_range);

  if (json)
  {
    fputs("{\n  \"device\": ", stdout);
    aud_json_string(stdout, uid);
    fputs(",\n  \"backend\": \"coreaudio\"", stdout);
    fputs(",\n  \"description\": ", stdout);
    aud_json_string(stdout, label);
    fputs(",\n  \"formats\": [\"S16_LE\", \"S24_3LE\", \"S24_LE\", \"S32_LE\"]", stdout);
    printf(",\n  \"channels\": {\"min\": %u, \"max\": %u}", channels > 0 ? 1u : 0u,
           channels);
    printf(",\n  \"rate\": %.0f", aud_ca_rate(device));
    fputs(",\n  \"rates\": [", stdout);
    for (unsigned i = 0; i < rate_count; i++)
    {
      printf("%s{\"min\": %.0f, \"max\": %.0f}", i > 0 ? ", " : "",
             (double)rates[i].mMinimum, (double)rates[i].mMaximum);
    }
    printf("]");
    printf(",\n  \"period_frames\": {\"min\": %.0f, \"max\": %.0f}",
           (double)period_range.mMinimum, (double)period_range.mMaximum);
    fputs(",\n  \"converted\": true\n}\n", stdout);
  }
  else
  {
    printf("device:   %s\n", uid);
    printf("backend:  coreaudio\n");
    printf("what:     %s\n", label);
    printf("formats:  S16_LE S24_3LE S24_LE S32_LE\n");
    printf("channels: 1..%u\n", channels);
    printf("rate:     %.0f Hz (what it is set to now)\n", aud_ca_rate(device));

    printf("rates:   ");
    for (unsigned i = 0; i < rate_count; i++)
    {
      if (rates[i].mMinimum == rates[i].mMaximum)
      {
        printf(" %.0f", (double)rates[i].mMinimum);
      }
      else
      {
        printf(" %.0f..%.0f", (double)rates[i].mMinimum, (double)rates[i].mMaximum);
      }
    }
    printf(" Hz\n");

    printf("period:   %.0f..%.0f frames\n", (double)period_range.mMinimum,
           (double)period_range.mMaximum);
    printf("\n");
    printf("The rate belongs to the device rather than to a recording, so asking\n");
    printf("for one of the others with -r moves it for everything else using this\n");
    printf("device. The formats are what audiaki writes: CoreAudio hands over\n");
    printf("float whatever the hardware is doing.\n");
  }

  free(rates);
  return 0;
}

/* -- the capture stream ---------------------------------------------------- */

typedef struct
{
  AudioUnit unit;
  AudioDeviceID device;
  int running;

  unsigned channels;
  unsigned rate;
  aud_format format;

  aud_ringbuf fifo; /* interleaved float, `channels` per frame */

  /*
   * Where AudioUnitRender() puts the period, before it goes into the ring. The
   * list is a header pointing at `staging`; both are sized once, at open, so
   * the I/O thread never allocates.
   */
  AudioBufferList list;
  float *staging;
  size_t staging_frames;

  /* the reader's own, for encoding out of the ring into the capture layout */
  float *decode;
  size_t decode_frames;

  /* only for the reader to sleep on; the callback never waits for it */
  pthread_mutex_t lock;
  pthread_cond_t cond;

  unsigned overruns;
  int broken;
  int stopped; /* set by drop(): stop taking what arrives */
} ca_capture;

/*
 * The device audiaki is recording from, gone: unplugged, or switched off.
 *
 * Without this the reader would sit on its timeout forever, being told nothing
 * arrived, on a device that is never going to send anything again. Ending the
 * stream instead is what the other three backends do, and what a take stopping
 * with everything written so far depends on.
 */
static const AudioObjectPropertyAddress device_alive = {kAudioDevicePropertyDeviceIsAlive,
                                                        kAudioObjectPropertyScopeGlobal,
                                                        AUD_CA_ELEMENT_MAIN};

/* Wake a reader that is asleep, without ever blocking the I/O thread. */
static void wake_reader(ca_capture *c)
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

static OSStatus capture_device_gone(AudioObjectID object, UInt32 count,
                                    const AudioObjectPropertyAddress *addresses,
                                    void *ref)
{
  ca_capture *c = ref;
  UInt32 alive = 0;

  (void)count;
  (void)addresses;

  if (aud_ca_get(object, kAudioDevicePropertyDeviceIsAlive,
                 kAudioObjectPropertyScopeGlobal, sizeof(alive), &alive) != 0 ||
      alive == 0)
  {
    c->broken = 1;
    wake_reader(c);
  }
  return noErr;
}

/*
 * The HAL has a period ready. Nothing is handed over with the call: an input
 * AUHAL wants to be asked, so the frames arrive through AudioUnitRender() into
 * a buffer this side owns.
 */
static OSStatus capture_render(void *ref, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *stamp, UInt32 bus, UInt32 frames,
                               AudioBufferList *io)
{
  ca_capture *c = ref;
  size_t samples;
  OSStatus err;

  (void)io; /* NULL for an input callback; the buffers below are ours */

  if (frames == 0 || c->stopped)
  {
    return noErr;
  }

  if ((size_t)frames > c->staging_frames)
  {
    /* the device moved to a longer period than it said it would */
    c->overruns++;
    return noErr;
  }

  c->list.mNumberBuffers = 1;
  c->list.mBuffers[0].mNumberChannels = c->channels;
  c->list.mBuffers[0].mDataByteSize =
      (UInt32)((size_t)frames * c->channels * sizeof(float));
  c->list.mBuffers[0].mData = c->staging;

  err = AudioUnitRender(c->unit, flags, stamp, bus, frames, &c->list);
  if (err != noErr)
  {
    c->overruns++;
    return noErr; /* one lost period, not a reason to tear the stream down */
  }

  samples = (size_t)frames * c->channels;

  /*
   * Checked before writing rather than writing what fits: a short write would
   * leave half a frame in the ring, and every frame after it would be read one
   * channel out of step.
   */
  if (aud_ringbuf_space(&c->fifo) < samples)
  {
    c->overruns++;
    return noErr;
  }

  aud_ringbuf_write(&c->fifo, c->staging, samples);
  wake_reader(c);
  return noErr;
}

static void capture_free(ca_capture *c)
{
  if (c == NULL)
  {
    return;
  }

  if (c->device != kAudioObjectUnknown)
  {
    AudioObjectRemovePropertyListener(c->device, &device_alive, capture_device_gone, c);
  }

  /*
   * Stop and dispose before freeing anything the callback touches.
   * AudioOutputUnitStop() does not return until the I/O thread is out of the
   * callback, which is what makes the frees below safe.
   */
  if (c->unit != NULL)
  {
    if (c->running)
    {
      AudioOutputUnitStop(c->unit);
      c->running = 0;
    }
    AudioComponentInstanceDispose(c->unit);
    c->unit = NULL;
  }

  aud_ringbuf_free(&c->fifo);
  pthread_cond_destroy(&c->cond);
  pthread_mutex_destroy(&c->lock);
  free(c->staging);
  free(c->decode);
  free(c);
}

static void coreaudio_close(aud_device *dev)
{
  if (dev->handle == NULL)
  {
    return;
  }

  capture_free((ca_capture *)dev->handle);
  dev->handle = NULL;
}

/* Turn the input element on and the output element off. Returns 0 on success. */
static int enable_input_only(AudioUnit unit)
{
  UInt32 on = 1;
  UInt32 off = 0;
  char status[16];
  OSStatus err;

  err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, CA_INPUT_BUS, &on, sizeof(on));
  if (err != noErr)
  {
    aud_error("coreaudio: cannot enable capture on the unit: %s",
              aud_ca_status_text(err, status, sizeof(status)));
    return -1;
  }

  err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, CA_OUTPUT_BUS, &off, sizeof(off));
  if (err != noErr)
  {
    aud_error("coreaudio: cannot disable playback on the unit: %s",
              aud_ca_status_text(err, status, sizeof(status)));
    return -1;
  }
  return 0;
}

/*
 * Take the first `channels` of the device's inputs.
 *
 * An AUHAL will not simply narrow a device's channel count on the way out of
 * its input element - it wants telling which of the hardware's channels the
 * stream's channels come from. Recording two of an eight input interface
 * without this gets a format error rather than the first two.
 */
static int set_channel_map(AudioUnit unit, unsigned channels)
{
  SInt32 *map = calloc(channels, sizeof(*map));
  OSStatus err;

  if (map == NULL)
  {
    return -1;
  }

  for (unsigned i = 0; i < channels; i++)
  {
    map[i] = (SInt32)i;
  }

  err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_ChannelMap,
                             kAudioUnitScope_Output, CA_INPUT_BUS, map,
                             (UInt32)(channels * sizeof(*map)));
  free(map);
  return err == noErr ? 0 : -1;
}

static int coreaudio_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  AudioStreamBasicDescription asbd;
  AURenderCallbackStruct callback;
  ca_capture *c;
  unsigned available;
  unsigned period;
  size_t fifo_frames;
  char status[16];
  char label[128];
  OSStatus err;

  c = calloc(1, sizeof(*c));
  if (c == NULL)
  {
    aud_error("out of memory opening the capture stream");
    return -1;
  }

  /*
   * CoreAudio is float end to end and has no integer format to ask for, so the
   * choice is only what audiaki writes into the file. S32_LE by default, which
   * is the one that gives the float it was handed nothing to round to.
   */
  c->format = cfg->format != AUD_FORMAT_UNKNOWN ? cfg->format : AUD_FORMAT_S32_LE;
  c->channels = cfg->channels;

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

  c->device = aud_ca_find_device(cfg->name, 1 /* input */);
  if (c->device == kAudioObjectUnknown)
  {
    aud_error("cannot open capture device '%s'",
              cfg->name != NULL ? cfg->name : AUD_DEFAULT_DEVICE);
    aud_info("run '" AUDIAKI_NAME " --list' to see the available capture devices");
    capture_free(c);
    return -1;
  }

  aud_ca_device_name(c->device, label, sizeof(label));

  available = aud_ca_channels(c->device, kAudioObjectPropertyScopeInput);
  if (available < c->channels)
  {
    aud_error("cannot set %u channel(s): %s has %u", c->channels, label, available);
    aud_info("device supports 1..%u channels", available);
    capture_free(c);
    return -1;
  }

  /*
   * The rate is the device's, so this moves it rather than negotiating one for
   * this stream alone. Declining to move it would mean recording at whatever
   * the last application left the device at, which is worse.
   */
  if (aud_ca_set_rate(c->device, (double)cfg->rate) != 0)
  {
    c->rate = (unsigned)(aud_ca_rate(c->device) + 0.5);
    aud_warn("requested %u Hz, %s is running at %u Hz", cfg->rate, label, c->rate);
  }
  else
  {
    c->rate = cfg->rate;
  }
  if (c->rate == 0)
  {
    aud_error("coreaudio: %s did not say what rate it is running at", label);
    capture_free(c);
    return -1;
  }

  period = aud_ca_set_period(c->device, cfg->period_frames);

  c->unit = aud_ca_new_unit("capture");
  if (c->unit == NULL)
  {
    capture_free(c);
    return -1;
  }

  if (enable_input_only(c->unit) != 0)
  {
    capture_free(c);
    return -1;
  }

  err = AudioUnitSetProperty(c->unit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &c->device, sizeof(c->device));
  if (err != noErr)
  {
    aud_error("cannot open capture device '%s': %s", label,
              aud_ca_status_text(err, status, sizeof(status)));
    capture_free(c);
    return -1;
  }

  if (c->channels < available && set_channel_map(c->unit, c->channels) != 0)
  {
    aud_warn("coreaudio: cannot pick %u of %s's %u inputs; taking what it gives",
             c->channels, label, available);
  }

  /*
   * The format the unit hands audiaki, which is the output scope of the input
   * element. The input scope of that element is the hardware's own and is not
   * ours to set.
   */
  asbd = aud_ca_stream_format((double)c->rate, c->channels);
  err = AudioUnitSetProperty(c->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, CA_INPUT_BUS, &asbd, sizeof(asbd));
  if (err != noErr)
  {
    aud_error("coreaudio: %s will not give %u channel(s) at %u Hz: %s", label,
              c->channels, c->rate, aud_ca_status_text(err, status, sizeof(status)));
    capture_free(c);
    return -1;
  }

  memset(&callback, 0, sizeof(callback));
  callback.inputProc = capture_render;
  callback.inputProcRefCon = c;
  err = AudioUnitSetProperty(c->unit, kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global, 0, &callback, sizeof(callback));
  if (err != noErr)
  {
    aud_error("coreaudio: cannot attach the capture callback: %s",
              aud_ca_status_text(err, status, sizeof(status)));
    capture_free(c);
    return -1;
  }

  /*
   * A period longer than the device asked for, because the HAL is entitled to
   * hand over more than kAudioDevicePropertyBufferFrameSize in a cycle and a
   * staging buffer that is exactly the right size turns that into a dropout.
   */
  c->staging_frames = (size_t)period * 2u;
  c->staging = malloc(c->staging_frames * c->channels * sizeof(*c->staging));

  /* the reader asks for its own period, and never gets more than it asked for */
  c->decode_frames = cfg->period_frames > period ? cfg->period_frames : period;
  c->decode = malloc(c->decode_frames * c->channels * sizeof(*c->decode));

  if (c->staging == NULL || c->decode == NULL)
  {
    aud_error("out of memory opening the capture stream");
    capture_free(c);
    return -1;
  }

  fifo_frames = (size_t)cfg->period_frames * cfg->periods;
  if (fifo_frames < CA_FIFO_MIN_FRAMES)
  {
    fifo_frames = CA_FIFO_MIN_FRAMES;
  }
  if (fifo_frames < (size_t)period * CA_FIFO_MIN_PERIODS)
  {
    fifo_frames = (size_t)period * CA_FIFO_MIN_PERIODS;
  }

  if (aud_ringbuf_init(&c->fifo, fifo_frames * c->channels) != 0)
  {
    aud_error("out of memory sizing the capture buffer");
    capture_free(c);
    return -1;
  }

  err = AudioUnitInitialize(c->unit);
  if (err != noErr)
  {
    aud_error("cannot open capture device '%s': %s", label,
              aud_ca_status_text(err, status, sizeof(status)));
    if (err == kAudioUnitErr_CannotDoInCurrentContext)
    {
      aud_info("the device is held exclusively by another program");
    }
    capture_free(c);
    return -1;
  }

  /* before the stream starts, so an unplug during the first period is caught */
  aud_ca_use_own_notify_thread();
  AudioObjectAddPropertyListener(c->device, &device_alive, capture_device_gone, c);

  err = AudioOutputUnitStart(c->unit);
  if (err != noErr)
  {
    aud_error("coreaudio: cannot start capture on %s: %s", label,
              aud_ca_status_text(err, status, sizeof(status)));
    aud_info("macOS asks for microphone permission the first time; check System "
             "Settings > Privacy & Security > Microphone");
    capture_free(c);
    return -1;
  }
  c->running = 1;

  dev->handle = c;
  dev->name = cfg->name;
  dev->format = c->format;
  dev->rate = c->rate;
  dev->channels = c->channels;
  dev->period_frames = cfg->period_frames;
  dev->buffer_frames = (unsigned long)fifo_frames;

  aud_debug("opened %s through coreaudio: %s, %u Hz, %u ch, device period %u frames, "
            "fifo %lu frames",
            label, aud_format_name(dev->format), dev->rate, dev->channels, period,
            dev->buffer_frames);
  return 0;
}

static long coreaudio_read(aud_device *dev, void *buf, unsigned long frames,
                           unsigned *xruns)
{
  ca_capture *c = (ca_capture *)dev->handle;
  size_t wanted = frames < c->decode_frames ? (size_t)frames : c->decode_frames;
  size_t available;
  size_t got;
  unsigned overruns;

  pthread_mutex_lock(&c->lock);

  while (aud_ringbuf_available(&c->fifo) < c->channels && !c->broken)
  {
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += CA_READ_TIMEOUT_NS;
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

  available = aud_ringbuf_available(&c->fifo) / c->channels;
  pthread_mutex_unlock(&c->lock);

  if (available == 0)
  {
    aud_error("capture stopped: the device is no longer available");
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

static void coreaudio_drop(aud_device *dev)
{
  ca_capture *c = (ca_capture *)dev->handle;

  if (c == NULL)
  {
    return;
  }

  /*
   * Stop the unit before emptying what it left, so the two do not race over the
   * same ring. Skipping rather than resetting, because only the reader may move
   * the read index and this is the reader.
   */
  c->stopped = 1;
  if (c->running)
  {
    AudioOutputUnitStop(c->unit);
    c->running = 0;
  }
  aud_ringbuf_skip(&c->fifo, aud_ringbuf_available(&c->fifo));
}

/* -- the hotplug watch ----------------------------------------------------- */

/*
 * The HAL says when a device arrives or goes, so like the PipeWire and JACK
 * watches there is nothing to poll and no settling delay to guess at.
 */
typedef struct
{
  pthread_mutex_t lock;
  int changed;
  int listening;
} ca_watch;

static const AudioObjectPropertyAddress watch_devices = {
    kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, AUD_CA_ELEMENT_MAIN};

static const AudioObjectPropertyAddress watch_default_input = {
    kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal,
    AUD_CA_ELEMENT_MAIN};

static OSStatus watch_changed_cb(AudioObjectID object, UInt32 count,
                                 const AudioObjectPropertyAddress *addresses, void *ref)
{
  ca_watch *w = ref;

  (void)object;
  (void)count;
  (void)addresses;

  pthread_mutex_lock(&w->lock);
  w->changed = 1;
  pthread_mutex_unlock(&w->lock);
  return noErr;
}

static void *coreaudio_watch_create(void)
{
  ca_watch *w = calloc(1, sizeof(*w));

  if (w == NULL)
  {
    return NULL;
  }

  if (pthread_mutex_init(&w->lock, NULL) != 0)
  {
    free(w);
    return NULL;
  }

  aud_ca_use_own_notify_thread();

  if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &watch_devices,
                                     watch_changed_cb, w) == noErr)
  {
    w->listening = 1;
  }
  else
  {
    aud_debug("coreaudio: cannot watch for devices; the list will not update");
  }

  /* a device disappearing moves the default, which is what -D default follows */
  AudioObjectAddPropertyListener(kAudioObjectSystemObject, &watch_default_input,
                                 watch_changed_cb, w);

  return w; /* a watch that never fires still satisfies the contract */
}

static void coreaudio_watch_destroy(void *impl)
{
  ca_watch *w = impl;

  if (w == NULL)
  {
    return;
  }

  AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &watch_devices,
                                    watch_changed_cb, w);
  AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &watch_default_input,
                                    watch_changed_cb, w);

  pthread_mutex_destroy(&w->lock);
  free(w);
}

static int coreaudio_watch_changed(void *impl)
{
  ca_watch *w = impl;
  int changed;

  if (w == NULL || !w->listening)
  {
    return 0;
  }

  pthread_mutex_lock(&w->lock);
  changed = w->changed;
  w->changed = 0;
  pthread_mutex_unlock(&w->lock);

  return changed;
}

const aud_capture_ops aud_capture_ops_coreaudio = {
    .name = "coreaudio",
    .open_capture = coreaudio_open_capture,
    .close = coreaudio_close,
    .read = coreaudio_read,
    .drop = coreaudio_drop,
    .probe = coreaudio_probe,
    .enumerate = coreaudio_enumerate,
    .watch_create = coreaudio_watch_create,
    .watch_destroy = coreaudio_watch_destroy,
    .watch_changed = coreaudio_watch_changed,
};
