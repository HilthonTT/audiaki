/* SPDX-License-Identifier: MIT */
/*
 * coreaudio_common.h - what the two CoreAudio backends both need.
 *
 * Internal to src/backend, and to the CoreAudio half of it: asking the HAL
 * about a device, and turning what it answers into the plain C strings and
 * numbers the rest of audiaki works in. device_coreaudio.c and
 * monitor_coreaudio.c each do their own job with these; nothing outside those
 * two files includes this.
 *
 * Inline in a header rather than a third translation unit for the same reason
 * jack_common.h is: this is property lookups with no state between them, and a
 * .c file would mean a build rule and an object for no gain.
 *
 * The HAL's own vocabulary leaks in one place worth naming. A "device" here is
 * an AudioDeviceID, which audiaki has to name in text for -D and for a --list
 * that means something tomorrow. The UID is that name: unlike the human
 * readable one it is unique, and unlike the ID it survives a reboot and a
 * replug.
 */
#ifndef AUDIAKI_COREAUDIO_COMMON_H
#define AUDIAKI_COREAUDIO_COMMON_H

#include "util/log.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>

/*
 * kAudioObjectPropertyElementMain in macOS 12 and later, and
 * kAudioObjectPropertyElementMaster before it. Both are zero, and spelling it
 * as the value rather than as either name is what lets one source build
 * against both SDKs without a version test that gets it wrong on one of them.
 */
#define AUD_CA_ELEMENT_MAIN ((AudioObjectPropertyElement)0)

/*
 * An OSStatus in words. Most of the interesting ones are four printable
 * characters read as an integer - 'nope', 'fmt?', '!dat' - so those are printed
 * back as the characters they are, and anything else as a number.
 *
 * `buf` should hold at least 16 bytes.
 */
static inline const char *aud_ca_status_text(OSStatus status, char *buf, size_t size)
{
  /*
   * Through the unsigned type first. An OSStatus is signed and plenty of them
   * are negative, and shifting a negative value right is implementation
   * defined - the same portability trap the WAV reader was fixed for.
   */
  UInt32 bits = (UInt32)status;
  unsigned char code[4];

  code[0] = (unsigned char)((bits >> 24) & 0xFFu);
  code[1] = (unsigned char)((bits >> 16) & 0xFFu);
  code[2] = (unsigned char)((bits >> 8) & 0xFFu);
  code[3] = (unsigned char)(bits & 0xFFu);

  for (int i = 0; i < 4; i++)
  {
    if (code[i] < 0x20 || code[i] > 0x7E)
    {
      snprintf(buf, size, "%d", (int)status);
      return buf;
    }
  }

  snprintf(buf, size, "'%c%c%c%c'", code[0], code[1], code[2], code[3]);
  return buf;
}

/*
 * Ask the HAL to deliver its notifications on a thread of its own.
 *
 * Left alone it may deliver them through the process's main run loop, which
 * audiaki does not run - neither the CLI nor the raylib window has one turning -
 * and a listener would simply never fire. Anything that registers one calls
 * this first; it is a property of the process, so saying it twice is harmless.
 */
static inline void aud_ca_use_own_notify_thread(void)
{
  AudioObjectPropertyAddress address = {kAudioHardwarePropertyRunLoop,
                                        kAudioObjectPropertyScopeGlobal,
                                        AUD_CA_ELEMENT_MAIN};
  CFRunLoopRef none = NULL;

  AudioObjectSetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, sizeof(none),
                             &none);
}

/* Read a fixed-size property. Returns 0 on success. */
static inline int aud_ca_get(AudioObjectID object, AudioObjectPropertySelector selector,
                             AudioObjectPropertyScope scope, UInt32 size, void *out)
{
  AudioObjectPropertyAddress address = {selector, scope, AUD_CA_ELEMENT_MAIN};

  return AudioObjectGetPropertyData(object, &address, 0, NULL, &size, out) == noErr ? 0
                                                                                    : -1;
}

/*
 * Read a variable-size property into memory this allocates. Returns the byte
 * count and stores the block in *out, which the caller frees; 0 with *out NULL
 * when the property is absent or empty.
 */
static inline UInt32 aud_ca_get_alloc(AudioObjectID object,
                                      AudioObjectPropertySelector selector,
                                      AudioObjectPropertyScope scope, void **out)
{
  AudioObjectPropertyAddress address = {selector, scope, AUD_CA_ELEMENT_MAIN};
  UInt32 size = 0;
  void *block;

  *out = NULL;

  if (AudioObjectGetPropertyDataSize(object, &address, 0, NULL, &size) != noErr ||
      size == 0)
  {
    return 0;
  }

  block = malloc(size);
  if (block == NULL)
  {
    return 0;
  }

  if (AudioObjectGetPropertyData(object, &address, 0, NULL, &size, block) != noErr)
  {
    free(block);
    return 0;
  }

  *out = block;
  return size;
}

/* Copy a CFString into `buf` as UTF-8. Leaves an empty string on failure. */
static inline void aud_ca_string(CFStringRef text, char *buf, size_t size)
{
  buf[0] = '\0';
  if (text == NULL)
  {
    return;
  }
  if (!CFStringGetCString(text, buf, (CFIndex)size, kCFStringEncodingUTF8))
  {
    buf[0] = '\0';
  }
}

/* A string-valued device property. Leaves an empty string when there is none. */
static inline void aud_ca_device_string(AudioDeviceID device,
                                        AudioObjectPropertySelector selector, char *buf,
                                        size_t size)
{
  CFStringRef text = NULL;

  buf[0] = '\0';
  if (aud_ca_get(device, selector, kAudioObjectPropertyScopeGlobal, sizeof(text),
                 &text) != 0 ||
      text == NULL)
  {
    return;
  }

  aud_ca_string(text, buf, size);
  CFRelease(text);
}

/* What -D takes and --list prints: stable across reboots, unlike the name. */
static inline void aud_ca_device_uid(AudioDeviceID device, char *buf, size_t size)
{
  aud_ca_device_string(device, kAudioDevicePropertyDeviceUID, buf, size);
}

/* What the device calls itself, for a human reading a listing. */
static inline void aud_ca_device_name(AudioDeviceID device, char *buf, size_t size)
{
  aud_ca_device_string(device, kAudioObjectPropertyName, buf, size);
}

/*
 * Channels the device has in `scope`, which is kAudioObjectPropertyScopeInput
 * for capture and ...ScopeOutput for playback. Zero means it does not do that
 * direction at all, which is how an output-only device is kept out of a list of
 * capture devices.
 */
static inline unsigned aud_ca_channels(AudioDeviceID device,
                                       AudioObjectPropertyScope scope)
{
  AudioBufferList *layout = NULL;
  unsigned channels = 0;

  if (aud_ca_get_alloc(device, kAudioDevicePropertyStreamConfiguration, scope,
                       (void **)&layout) == 0)
  {
    return 0;
  }

  for (UInt32 i = 0; i < layout->mNumberBuffers; i++)
  {
    channels += layout->mBuffers[i].mNumberChannels;
  }

  free(layout);
  return channels;
}

/* Every device the machine has. Returns the count; the caller frees *out. */
static inline unsigned aud_ca_devices(AudioDeviceID **out)
{
  UInt32 bytes = aud_ca_get_alloc(kAudioObjectSystemObject, kAudioHardwarePropertyDevices,
                                  kAudioObjectPropertyScopeGlobal, (void **)out);

  return (unsigned)(bytes / sizeof(AudioDeviceID));
}

/* The system's default device for the direction, or kAudioObjectUnknown. */
static inline AudioDeviceID aud_ca_default_device(int for_input)
{
  AudioDeviceID device = kAudioObjectUnknown;
  AudioObjectPropertySelector selector = for_input
                                             ? kAudioHardwarePropertyDefaultInputDevice
                                             : kAudioHardwarePropertyDefaultOutputDevice;

  if (aud_ca_get(kAudioObjectSystemObject, selector, kAudioObjectPropertyScopeGlobal,
                 sizeof(device), &device) != 0)
  {
    return kAudioObjectUnknown;
  }
  return device;
}

/*
 * Find the device `name` asks for, or kAudioObjectUnknown.
 *
 * NULL and "default" mean the system default for the direction. Anything else
 * is matched against the UID first and the human readable name second, because
 * the UID is what --list prints and what a script should be pinning, while the
 * name is what somebody typing it from memory will reach for. Both are compared
 * whole, and the name without regard to case.
 */
static inline AudioDeviceID aud_ca_find_device(const char *name, int for_input)
{
  AudioDeviceID *devices = NULL;
  AudioDeviceID found = kAudioObjectUnknown;
  AudioObjectPropertyScope scope =
      for_input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
  unsigned count;

  if (name == NULL || strcmp(name, "default") == 0)
  {
    return aud_ca_default_device(for_input);
  }

  count = aud_ca_devices(&devices);
  for (unsigned i = 0; i < count; i++)
  {
    char uid[128];

    if (aud_ca_channels(devices[i], scope) == 0)
    {
      continue;
    }

    aud_ca_device_uid(devices[i], uid, sizeof(uid));
    if (uid[0] != '\0' && strcmp(uid, name) == 0)
    {
      found = devices[i];
      break;
    }
  }

  if (found == kAudioObjectUnknown)
  {
    for (unsigned i = 0; i < count; i++)
    {
      char label[128];

      if (aud_ca_channels(devices[i], scope) == 0)
      {
        continue;
      }

      aud_ca_device_name(devices[i], label, sizeof(label));
      if (label[0] != '\0' && strcasecmp(label, name) == 0)
      {
        found = devices[i];
        break;
      }
    }
  }

  free(devices);
  return found;
}

/* The rate the device is running at now, or 0. */
static inline double aud_ca_rate(AudioDeviceID device)
{
  Float64 rate = 0.0;

  if (aud_ca_get(device, kAudioDevicePropertyNominalSampleRate,
                 kAudioObjectPropertyScopeGlobal, sizeof(rate), &rate) != 0)
  {
    return 0.0;
  }
  return (double)rate;
}

/* Whether the device will run at `rate`. */
static inline int aud_ca_rate_supported(AudioDeviceID device, double rate)
{
  AudioValueRange *ranges = NULL;
  UInt32 bytes = aud_ca_get_alloc(device, kAudioDevicePropertyAvailableNominalSampleRates,
                                  kAudioObjectPropertyScopeGlobal, (void **)&ranges);
  unsigned count = (unsigned)(bytes / sizeof(AudioValueRange));
  int ok = 0;

  for (unsigned i = 0; i < count && !ok; i++)
  {
    /*
     * A range rather than a value, because a device with a continuously
     * variable clock reports one. Most report a range per rate, where the two
     * ends are equal.
     */
    ok = rate >= (double)ranges[i].mMinimum - 0.5 &&
         rate <= (double)ranges[i].mMaximum + 0.5;
  }

  free(ranges);
  return ok;
}

/*
 * Ask the device to run at `rate`. Returns 0 when it is running at it
 * afterwards, whether or not this call is what got it there.
 *
 * The rate is a property of the device rather than of the stream, so this moves
 * it for everything on the machine that is using it. That is the CoreAudio
 * bargain: there is no per-client rate to set, and a recorder that would not
 * change it could only ever record at whatever the last application left it at.
 */
static inline int aud_ca_set_rate(AudioDeviceID device, double rate)
{
  AudioObjectPropertyAddress address = {kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal,
                                        AUD_CA_ELEMENT_MAIN};
  Float64 wanted = (Float64)rate;

  if (aud_ca_rate(device) == rate)
  {
    return 0;
  }
  if (!aud_ca_rate_supported(device, rate))
  {
    return -1;
  }

  if (AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof(wanted), &wanted) !=
      noErr)
  {
    return -1;
  }

  /*
   * The change is not instant: the HAL restarts the device's I/O around it, and
   * asking straight away can still see the old rate. A short wait beats
   * reporting a rate the stream will not actually be given.
   */
  for (int i = 0; i < 40 && aud_ca_rate(device) != rate; i++)
  {
    struct timespec ts = {0, 5000000L}; /* 5 ms */

    nanosleep(&ts, NULL);
  }

  return aud_ca_rate(device) == rate ? 0 : -1;
}

/*
 * Ask the device for a period of `frames`, clamped to what it allows. Not an
 * error when it declines: the size only decides how often the callback runs.
 */
static inline unsigned aud_ca_set_period(AudioDeviceID device, unsigned frames)
{
  AudioObjectPropertyAddress address = {kAudioDevicePropertyBufferFrameSize,
                                        kAudioObjectPropertyScopeGlobal,
                                        AUD_CA_ELEMENT_MAIN};
  AudioValueRange range;
  UInt32 wanted = (UInt32)frames;
  UInt32 actual = 0;

  if (aud_ca_get(device, kAudioDevicePropertyBufferFrameSizeRange,
                 kAudioObjectPropertyScopeGlobal, sizeof(range), &range) == 0)
  {
    if ((double)wanted < range.mMinimum)
    {
      wanted = (UInt32)range.mMinimum;
    }
    else if ((double)wanted > range.mMaximum)
    {
      wanted = (UInt32)range.mMaximum;
    }
  }

  AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof(wanted), &wanted);

  if (aud_ca_get(device, kAudioDevicePropertyBufferFrameSize,
                 kAudioObjectPropertyScopeGlobal, sizeof(actual), &actual) != 0)
  {
    return frames;
  }
  return (unsigned)actual;
}

/*
 * The interleaved float stream audiaki asks an AUHAL for, in both directions.
 *
 * Float because that is what CoreAudio works in - asking for anything else buys
 * a conversion inside the unit rather than avoiding one - and interleaved
 * because everything above device.h reads interleaved frames.
 */
static inline AudioStreamBasicDescription aud_ca_stream_format(double rate,
                                                               unsigned channels)
{
  AudioStreamBasicDescription asbd;

  memset(&asbd, 0, sizeof(asbd));
  asbd.mSampleRate = (Float64)rate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  asbd.mBitsPerChannel = 32;
  asbd.mChannelsPerFrame = (UInt32)channels;
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = (UInt32)(sizeof(float) * channels);
  asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  return asbd;
}

/*
 * A new AUHAL unit, uninitialised and with neither direction enabled yet.
 * Returns NULL after saying why.
 */
static inline AudioUnit aud_ca_new_unit(const char *what)
{
  AudioComponentDescription desc;
  AudioComponent component;
  AudioUnit unit = NULL;
  OSStatus err;
  char status[16];

  memset(&desc, 0, sizeof(desc));
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  component = AudioComponentFindNext(NULL, &desc);
  if (component == NULL)
  {
    aud_error("coreaudio: the system has no HAL output unit for %s", what);
    return NULL;
  }

  err = AudioComponentInstanceNew(component, &unit);
  if (err != noErr)
  {
    aud_error("coreaudio: cannot create the %s unit: %s", what,
              aud_ca_status_text(err, status, sizeof(status)));
    return NULL;
  }
  return unit;
}

#endif /* AUDIAKI_COREAUDIO_COMMON_H */
