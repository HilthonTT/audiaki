/* SPDX-License-Identifier: MIT */
#include "cmd/cmd.h"

#include "audio/tuner.h"
#include "term/meter.h"
#include "util/log.h"
#include "util/signals.h"

#include <stdint.h>
#include <stdlib.h>

/*
 * What a tuning run is asked for. Filled in from aud_options by aud_cmd_tune()
 * at the bottom of this file. The pitch detection itself is in audio/tuner.c, which
 * knows nothing about any audio system and is shared with the desktop app.
 */
typedef struct
{
  double a4_hz; /* what A above middle C is being called */
  /* the pitch range to search, or 0.0 at either end for the tuner's default */
  double min_hz;
  double max_hz;
  int show_meter; /* draw the live needle at all */
} aud_tune_options;

/*
 * Read from `dev` and display the pitch until interrupted. Returns 0 on
 * success and -1 on failure, after reporting the reason through log.h.
 */
static int tune_run(aud_device *dev, const aud_tune_options *opts);

/*
 * How often the pitch is analysed, in seconds of captured audio. Twenty times a
 * second is faster than a needle can usefully be read and far cheaper than the
 * detection would be at frame rate.
 */
#define TUNE_INTERVAL 0.05

/*
 * With no terminal to redraw in place, say something only when the note
 * changes. `audiaki --tune > log` should be a record of what was played, not
 * twenty identical lines a second.
 */
static void report_change(const aud_tuner_reading *reading, int *last_midi)
{
  char label[AUD_TUNER_LABEL_MAX];

  if (!reading->voiced || reading->confidence < 0.5)
  {
    *last_midi = -1;
    return;
  }
  if (reading->midi == *last_midi)
  {
    return;
  }

  *last_midi = reading->midi;
  aud_tuner_note_label(reading, label, sizeof(label));
  aud_info("%s  %+.0f cents  %.1f Hz", label, reading->cents, reading->frequency);
}

static int tune_run(aud_device *dev, const aud_tune_options *opts)
{
  aud_meter meter;
  aud_tuner_config cfg;
  aud_tuner *tuner = NULL;
  unsigned char *buf = NULL;
  size_t buf_bytes;
  uint64_t frames = 0;
  unsigned xruns = 0;
  double next_at = 0.0;
  double last_at = 0.0;
  int last_midi = -1;
  int rc = -1;

  buf_bytes = aud_device_period_bytes(dev);
  if (buf_bytes == 0)
  {
    aud_error("unsupported capture format");
    return -1;
  }

  buf = malloc(buf_bytes);
  if (buf == NULL)
  {
    aud_error("cannot allocate a %zu byte capture buffer", buf_bytes);
    return -1;
  }

  /* initialised before any early exit so the cleanup path is unconditional */
  meter_init(&meter, opts->show_meter);

  aud_tuner_config_defaults(&cfg, dev->rate);
  cfg.a4_hz = opts->a4_hz;
  if (opts->min_hz > 0.0)
  {
    cfg.min_hz = opts->min_hz;
  }
  if (opts->max_hz > 0.0)
  {
    cfg.max_hz = opts->max_hz;
  }
  /*
   * Nothing above Nyquist exists to be found, and a device that settled on a
   * lower rate than was asked for can put a range that was fine into that
   * position. Trimmed rather than refused: the rest of the range still works.
   */
  if (cfg.max_hz > (double)dev->rate * 0.45)
  {
    aud_warn("--tune-max %.4g is above what a %u Hz stream can carry; looking up to "
             "%.0f Hz instead",
             cfg.max_hz, dev->rate, (double)dev->rate * 0.45);
    cfg.max_hz = (double)dev->rate * 0.45;
  }

  tuner = aud_tuner_create(&cfg);
  if (tuner == NULL)
  {
    aud_error("cannot set up the tuner for a %u Hz stream over %.4g to %.4g Hz",
              dev->rate, cfg.min_hz, cfg.max_hz);
    goto out;
  }

  aud_info("tuning from %s: %u Hz, %u ch, A4 = %.1f Hz, looking from %.4g to %.4g Hz",
           dev->name, dev->rate, dev->channels, cfg.a4_hz, cfg.min_hz, cfg.max_hz);
  aud_debug("analysis window %zu frames (%.0f ms)", aud_tuner_window(tuner),
            1000.0 * (double)aud_tuner_window(tuner) / dev->rate);
  aud_info("press Ctrl+C to stop");

  while (!aud_signals_stop_requested())
  {
    aud_tuner_reading reading;
    long got = aud_device_read(dev, buf, dev->period_frames, &xruns);
    double elapsed;

    if (got < 0)
    {
      goto out;
    }
    if (got == 0)
    {
      continue;
    }

    aud_tuner_push_pcm(tuner, buf, (size_t)got, dev->channels, dev->format);

    frames += (uint64_t)got;
    elapsed = (double)frames / dev->rate;

    if (elapsed < next_at)
    {
      continue;
    }

    /*
     * Smooth against captured time rather than TUNE_INTERVAL, so the needle
     * settles at the same speed whether or not an analysis was skipped.
     */
    aud_tuner_analyse(tuner, elapsed - last_at, &reading);

    if (meter.enabled)
    {
      meter_draw_tuner(&meter, &reading);
    }
    else
    {
      report_change(&reading, &last_midi);
    }

    last_at = elapsed;
    next_at = elapsed + TUNE_INTERVAL;
  }

  rc = 0;

out:
  meter_clear(&meter);
  aud_tuner_destroy(tuner);
  free(buf);

  /* xruns are not worth a warning here: nothing was being written to lose */
  aud_debug("%u xrun(s) while tuning", xruns);
  return rc;
}

int aud_cmd_tune(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
  aud_tune_options tune;
  int rc;

  aud_cmd_capture_config(&cfg, opts);

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  if (aud_device_open_capture(&dev, &cfg) != 0)
  {
    return EXIT_FAILURE;
  }

  tune.a4_hz = opts->a4_hz;
  tune.min_hz = opts->tune_min_hz;
  tune.max_hz = opts->tune_max_hz;
  tune.show_meter = opts->show_meter;

  rc = tune_run(&dev, &tune);
  aud_device_close(&dev);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
