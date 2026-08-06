/* SPDX-License-Identifier: MIT */
/*
 * device.c - dispatch to the selected capture backend.
 *
 * Everything here is either a one-line forward into backend.h's table or logic
 * that is the same whichever backend answered - the defaults, the arithmetic,
 * and printing a device list that was already gathered.
 */
#include "device.h"

#include "backend.h"
#include "jsonout.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The watch a caller holds. The backend's own handle sits inside, alongside the
 * table that made it, so destroying a watch after the backend has been switched
 * still goes to the code that created it.
 */
struct aud_device_watch
{
  const aud_capture_ops *ops;
  void *impl;
};

void aud_device_config_defaults(aud_device_config *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->name = AUD_DEFAULT_DEVICE;
  cfg->rate = AUD_DEFAULT_RATE;
  cfg->channels = AUD_DEFAULT_CHANNELS;
  cfg->format = AUD_FORMAT_UNKNOWN;
  cfg->period_frames = AUD_DEFAULT_PERIOD_FRAMES;
  cfg->periods = AUD_DEFAULT_PERIODS;
}

int aud_device_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  const aud_capture_ops *ops = aud_backend_capture();

  memset(dev, 0, sizeof(*dev));
  dev->ops = ops;

  if (ops->open_capture(dev, cfg) != 0)
  {
    /* the backend has already said why, and left nothing to release */
    dev->ops = NULL;
    return -1;
  }
  return 0;
}

void aud_device_close(aud_device *dev)
{
  if (dev == NULL || dev->ops == NULL)
  {
    return;
  }

  dev->ops->close(dev);
  dev->ops = NULL;
}

long aud_device_read(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns)
{
  return dev->ops->read(dev, buf, frames, xruns);
}

void aud_device_drop(aud_device *dev)
{
  if (dev == NULL || dev->ops == NULL || dev->ops->drop == NULL)
  {
    return;
  }

  dev->ops->drop(dev);
}

size_t aud_device_period_bytes(const aud_device *dev)
{
  return (size_t)dev->period_frames * dev->channels * aud_format_hw_bytes(dev->format);
}

int aud_device_probe(const char *name, int json)
{
  return aud_backend_capture()->probe(name, json);
}

int aud_device_enumerate(aud_device_entry **out)
{
  return aud_backend_capture()->enumerate(out);
}

aud_device_watch *aud_device_watch_create(void)
{
  const aud_capture_ops *ops = aud_backend_capture();
  aud_device_watch *w = calloc(1, sizeof(*w));

  if (w == NULL)
  {
    return NULL;
  }

  w->ops = ops;
  w->impl = ops->watch_create();
  if (w->impl == NULL)
  {
    free(w);
    return NULL;
  }
  return w;
}

void aud_device_watch_destroy(aud_device_watch *w)
{
  if (w == NULL)
  {
    return;
  }

  w->ops->watch_destroy(w->impl);
  free(w);
}

int aud_device_watch_changed(aud_device_watch *w)
{
  if (w == NULL)
  {
    return 0;
  }

  return w->ops->watch_changed(w->impl);
}

int aud_device_list(int json)
{
  aud_device_entry *list = NULL;
  int found = aud_device_enumerate(&list);

  if (found < 0)
  {
    return -1;
  }

  if (json)
  {
    fputs("[", stdout);
  }
  else
  {
    printf("%-32s %s\n", "DEVICE", "DESCRIPTION");
  }

  for (int i = 0; i < found; i++)
  {
    if (json)
    {
      fputs(i == 0 ? "\n  {\"device\": " : ",\n  {\"device\": ", stdout);
      aud_json_string(stdout, list[i].name);
      fputs(", \"card\": ", stdout);
      aud_json_string(stdout, list[i].card);
      fputs(", \"description\": ", stdout);
      aud_json_string(stdout, list[i].description);
      fputs(", \"backend\": ", stdout);
      aud_json_string(stdout, aud_backend_capture()->name);
      fputc('}', stdout);
    }
    else
    {
      printf("%-32s %s: %s\n", list[i].name, list[i].card, list[i].description);
    }
  }

  if (json)
  {
    fputs(found > 0 ? "\n]\n" : "]\n", stdout);
  }
  else if (found == 0)
  {
    aud_warn("no capture devices found");
  }

  free(list);
  return 0;
}
