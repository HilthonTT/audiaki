/* SPDX-License-Identifier: MIT */
/*
 * devices.c - the capture devices the dropdown offers.
 *
 * Kept level with the hardware while the window is open, so plugging an
 * interface in puts it in the menu without the app being restarted around it.
 * The enumeration itself is backend/device.c; what is here is which rows to
 * show, and when swapping the list under a pointer is safe.
 */
#include "gui/app.h"

#include "backend/device.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build the list the device dropdown offers: "default" first, because it is
 * what works without knowing anything, then every capture PCM ALSA found.
 * A failed enumeration is not fatal - "default" alone is still a usable app.
 *
 * `keep` is a device that has to appear whether ALSA reports it or not: the
 * one -D named before it was plugged in, or the one the window is still
 * pointed at after its hardware was pulled out. Dropping the row would leave
 * the menu showing a device other than the one in use.
 */
static void app_build_devices(app_devices *d, const char *keep)
{
  aud_device_entry *found = NULL;
  int count;

  memset(d, 0, sizeof(*d));
  d->absent = -1;

  snprintf(d->name[0], sizeof(d->name[0]), "%s", AUD_DEFAULT_DEVICE);
  snprintf(d->label[0], sizeof(d->label[0]), "default (system)");
  d->count = 1;

  count = aud_device_enumerate(&found);
  for (int i = 0; i < count && d->count < APP_MAX_DEVICES; i++)
  {
    int slot = d->count;

    snprintf(d->name[slot], sizeof(d->name[slot]), "%s", found[i].name);
    if (found[i].description[0] != '\0')
    {
      snprintf(d->label[slot], sizeof(d->label[slot]), "%s: %s", found[i].card,
               found[i].description);
    }
    else
    {
      snprintf(d->label[slot], sizeof(d->label[slot]), "%s", found[i].card);
    }
    d->count++;
  }
  free(found);

  if (keep == NULL || keep[0] == '\0' || d->count == APP_MAX_DEVICES)
  {
    return;
  }

  for (int i = 0; i < d->count; i++)
  {
    if (strcmp(keep, d->name[i]) == 0)
    {
      return;
    }
  }

  snprintf(d->name[d->count], sizeof(d->name[d->count]), "%s", keep);
  snprintf(d->label[d->count], sizeof(d->label[d->count]), "%s (not connected)", keep);
  d->absent = d->count;
  d->count++;
}

/*
 * Take `next` as the list on offer and point the selection back at the device
 * in use, which may have moved rows or - the first time round - may be the one
 * -D asked for. The dropdown opens showing what is actually being captured
 * rather than the top of the list.
 */
static void app_adopt_devices(app *a, const app_devices *next)
{
  a->devices = *next;
  a->device_selected = 0;

  for (int i = 0; i < a->devices.count; i++)
  {
    a->device_labels[i] = a->devices.label[i];
    if (strcmp(a->active_device, a->devices.name[i]) == 0)
    {
      a->device_selected = i;
    }
  }

  if (a->devices.count == APP_MAX_DEVICES)
  {
    aud_warn("more than %d capture devices; the rest are not offered in the window",
             APP_MAX_DEVICES - 1);
  }
}

void app_load_devices(app *a)
{
  app_devices next;

  app_build_devices(&next, a->active_device);
  app_adopt_devices(a, &next);
}

/*
 * Re-walk ALSA. Returns 1 when the list changed, which is the only time it is
 * swapped in: the walk happens every couple of seconds, and rebuilding on a
 * timer would shuffle rows under a pointer that is about to click one.
 */
int app_refresh_devices(app *a)
{
  app_devices next;

  app_build_devices(&next, a->active_device);

  if (next.count == a->devices.count)
  {
    int same = 1;

    for (int i = 0; i < next.count && same; i++)
    {
      same = strcmp(next.name[i], a->devices.name[i]) == 0 &&
             strcmp(next.label[i], a->devices.label[i]) == 0;
    }
    if (same)
    {
      return 0;
    }
  }

  aud_debug("capture devices changed: %d offered", next.count);
  app_adopt_devices(a, &next);
  return 1;
}