/* SPDX-License-Identifier: MIT */
/*
 * device.h - capture device handling.
 *
 * The interface every caller uses, with no audio system in it: the handle below
 * is opaque, and which of ALSA or PipeWire is behind it is backend.h's business.
 * device.c is a dispatcher; device_alsa.c and device_pipewire.c do the work.
 */
#ifndef AUDIAKI_DEVICE_H
#define AUDIAKI_DEVICE_H

#include "backend.h"
#include "format.h"

#include <stddef.h>

#define AUD_DEFAULT_DEVICE "default"
#define AUD_DEFAULT_RATE 44100u
#define AUD_DEFAULT_CHANNELS 2u
#define AUD_DEFAULT_PERIOD_FRAMES 1024u
#define AUD_DEFAULT_PERIODS 4u

struct aud_device_config
{
  const char *name;       /* device string, in the backend's own spelling */
  unsigned rate;          /* requested sample rate in Hz */
  unsigned channels;      /* requested channel count */
  aud_format format;      /* AUD_FORMAT_UNKNOWN picks the best available */
  unsigned period_frames; /* requested period size */
  unsigned periods;       /* periods per buffer */
};

struct aud_device
{
  void *handle;               /* the backend's stream; NULL when closed */
  const aud_capture_ops *ops; /* the backend that opened it */
  const char *name;
  aud_format format;
  unsigned rate;     /* rate the device actually accepted */
  unsigned channels; /* channels the device actually accepted */
  unsigned long period_frames;
  unsigned long buffer_frames;
};

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

/*
 * Discard whatever is still queued and stop the stream, for the end of a take.
 * The device stays open. Safe on a zeroed or closed device.
 */
void aud_device_drop(aud_device *dev);

/* Bytes one full period occupies in the capture format. */
size_t aud_device_period_bytes(const aud_device *dev);

/*
 * Print the capabilities of `name` to stdout, as aligned text or as a JSON
 * object when `json` is non-zero. Returns 0 on success.
 */
int aud_device_probe(const char *name, int json);

/*
 * Print every capture-capable device to stdout, as a table or as a JSON array
 * when `json` is non-zero. Returns 0 on success.
 */
int aud_device_list(int json);

/* One capture-capable device, as found by aud_device_enumerate(). */
struct aud_device_entry
{
  char name[64];        /* the string to pass as a device */
  char card[80];        /* the card's human readable name */
  char description[80]; /* what the device calls itself */
};

/*
 * Build an array of every capture-capable device. Returns the number found and
 * stores the array in *out, which the caller frees; *out is NULL when nothing
 * was found. Returns -1 on failure.
 *
 * Under ALSA the plugin devices - "default", "pulse" and friends - are not
 * included, because they are configuration rather than hardware and ALSA offers
 * no way to enumerate them meaningfully. A caller offering a choice should
 * present "default" itself.
 */
int aud_device_enumerate(aud_device_entry **out);

/*
 * A watcher for capture hardware arriving and leaving, so a long-running
 * program can keep a device list current instead of asking the user to restart
 * it after plugging something in.
 *
 * How it notices is the backend's business - ALSA watches /dev/snd and sweeps,
 * PipeWire is told by the server - but the contract is the same either way, and
 * a caller only ever needs the one code path.
 */

/*
 * Start watching. Returns NULL only when out of memory - a watch that could
 * not attach to its source still works, it just polls.
 */
aud_device_watch *aud_device_watch_create(void);

/* Stop watching. Safe on NULL. */
void aud_device_watch_destroy(aud_device_watch *w);

/*
 * Non-blocking: returns 1 when the device list is worth rebuilding and 0 when
 * nothing has happened. Safe on NULL, which never reports a change.
 *
 * A burst of events is reported once, a moment after it settles - plugging in
 * one interface creates several nodes, and the card is only worth enumerating
 * when the kernel has finished with all of them.
 */
int aud_device_watch_changed(aud_device_watch *w);

#endif /* AUDIAKI_DEVICE_H */
