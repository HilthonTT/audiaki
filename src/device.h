/* SPDX-License-Identifier: MIT */
/*
 * device.h - ALSA capture device handling.
 *
 * This is the only translation unit pair that talks to libasound; everything
 * else works in terms of aud_format and plain buffers.
 */
#ifndef AUDIAKI_DEVICE_H
#define AUDIAKI_DEVICE_H

#include "format.h"

#include <alsa/asoundlib.h>
#include <stddef.h>

#define AUD_DEFAULT_DEVICE "default"
#define AUD_DEFAULT_RATE 44100u
#define AUD_DEFAULT_CHANNELS 2u
#define AUD_DEFAULT_PERIOD_FRAMES 1024u
#define AUD_DEFAULT_PERIODS 4u

typedef struct
{
  const char *name;       /* ALSA device string */
  unsigned rate;          /* requested sample rate in Hz */
  unsigned channels;      /* requested channel count */
  aud_format format;      /* AUD_FORMAT_UNKNOWN picks the best available */
  unsigned period_frames; /* requested period size */
  unsigned periods;       /* periods per buffer */
} aud_device_config;

typedef struct
{
  snd_pcm_t *pcm;
  const char *name;
  aud_format format;
  unsigned rate;     /* rate the device actually accepted */
  unsigned channels; /* channels the device actually accepted */
  unsigned long period_frames;
  unsigned long buffer_frames;
} aud_device;

/* Fill `cfg` with the AUD_DEFAULT_* values. */
void aud_device_config_defaults(aud_device_config *cfg);

/*
 * Open and configure a capture stream. Returns 0 on success and -1 after
 * reporting the failure through log.h. On success the caller owns `dev` and
 * must call aud_device_close().
 */
int aud_device_open_capture(aud_device *dev, const aud_device_config *cfg);

/* Release the stream. Safe to call on a zeroed or already closed device. */
void aud_device_close(aud_device *dev);

/*
 * Read up to `frames` frames. Returns the frame count on success, 0 when the
 * caller should simply retry (a recovered xrun or an interrupted read), or -1
 * on an unrecoverable error. `*xruns` is incremented on every overrun.
 */
long aud_device_read(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns);

/* Bytes one full period occupies in the capture format. */
size_t aud_device_period_bytes(const aud_device *dev);

/*
 * Print the capabilities of `name` to stdout, as aligned text or as a JSON
 * object when `json` is non-zero. Returns 0 on success.
 */
int aud_device_probe(const char *name, int json);

/*
 * Print every capture-capable PCM device to stdout, as a table or as a JSON
 * array when `json` is non-zero. Returns 0 on success.
 */
int aud_device_list(int json);

/* One capture-capable PCM, as found by aud_device_enumerate(). */
typedef struct
{
  char name[64];        /* the string to pass as a device, hw:CARD=x,DEV=n */
  char card[80];        /* the card's human readable name */
  char description[80]; /* what the PCM calls itself */
} aud_device_entry;

/*
 * Build an array of every capture-capable PCM. Returns the number found and
 * stores the array in *out, which the caller frees; *out is NULL when nothing
 * was found. Returns -1 on failure.
 *
 * The plugin devices - "default", "pulse" and friends - are not included,
 * because they are configuration rather than hardware and ALSA offers no way
 * to enumerate them meaningfully. A caller offering a choice should present
 * "default" itself.
 */
int aud_device_enumerate(aud_device_entry **out);

#endif /* AUDIAKI_DEVICE_H */
