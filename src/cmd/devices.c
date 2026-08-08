/* SPDX-License-Identifier: MIT */
/*
 * --list and --probe: what this machine can record from.
 *
 * The enumeration is backend/device.c, which asks whichever audio system was
 * selected. Laying the answer out is here, because a table is a presentation
 * decision and the backends have no business making one. --probe still prints
 * from inside the backend: what a device supports differs enough between ALSA
 * and PipeWire that there is no shared table to draw. See DESIGN.md.
 *
 * Also the one place that maps the capture options onto a device config, so
 * --record and --tune cannot drift apart in what -r or -p mean.
 */
#include "cmd/cmd.h"

#include "util/jsonout.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aud_cmd_capture_config(aud_device_config *cfg, const aud_options *opts)
{
  aud_device_config_defaults(cfg);
  cfg->name = opts->device;
  cfg->rate = opts->rate;
  cfg->channels = opts->channels;
  cfg->format = opts->format;
  cfg->period_frames = opts->period_frames;
  cfg->periods = opts->periods;
}

/*
 * Width of the DEVICE column. ALSA's `hw:CARD=x,DEV=n` fits the historic 32
 * comfortably; PipeWire's node names run past 40, and a fixed column would push
 * every description out of line. Widening to the longest name keeps the table a
 * table, and the floor means the ALSA listing prints exactly as it always has.
 */
static int list_name_width(const aud_device_entry *list, int count)
{
  int width = 32;

  for (int i = 0; i < count; i++)
  {
    int len = (int)strlen(list[i].name);

    if (len > width)
    {
      width = len;
    }
  }

  return width;
}

int aud_cmd_list(const aud_options *opts)
{
  aud_device_entry *list = NULL;
  int found = aud_device_enumerate(&list);
  int json = opts->json;
  int width;

  if (found < 0)
  {
    return EXIT_FAILURE;
  }

  width = list_name_width(list, found);

  if (json)
  {
    fputs("[", stdout);
  }
  else
  {
    printf("%-*s %s\n", width, "DEVICE", "DESCRIPTION");
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
      aud_json_string(stdout, aud_device_backend_name());
      fputc('}', stdout);
    }
    else
    {
      printf("%-*s %s: %s\n", width, list[i].name, list[i].card, list[i].description);
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
  return EXIT_SUCCESS;
}

int aud_cmd_probe(const aud_options *opts)
{
  return aud_device_probe(opts->device, opts->json) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
