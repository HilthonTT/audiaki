/* SPDX-License-Identifier: MIT */
#include "tune.h"

#include "log.h"
#include "meter.h"
#include "signals.h"
#include "tuner.h"

#include <stdint.h>
#include <stdlib.h>

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
    return;

  *last_midi = reading->midi;
  aud_tuner_note_label(reading, label, sizeof(label));
  aud_info("%s  %+.0f cents  %.1f Hz", label, reading->cents, reading->frequency);
}

int aud_tune_run(aud_device *dev, const aud_tune_options *opts)
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

  tuner = aud_tuner_create(&cfg);
  if (tuner == NULL)
  {
    aud_error("cannot set up the tuner for a %u Hz stream", dev->rate);
    goto out;
  }

  aud_info("tuning from %s: %u Hz, %u ch, A4 = %.1f Hz", dev->name, dev->rate,
           dev->channels, cfg.a4_hz);
  aud_debug("analysis window %zu frames (%.0f ms)", aud_tuner_window(tuner),
            1000.0 * (double)aud_tuner_window(tuner) / dev->rate);
  aud_info("press Ctrl+C to stop");

  while (!aud_signals_stop_requested())
  {
    aud_tuner_reading reading;
    long got = aud_device_read(dev, buf, dev->period_frames, &xruns);
    double elapsed;

    if (got < 0)
      goto out;
    if (got == 0)
      continue;

    aud_tuner_push_pcm(tuner, buf, (size_t)got, dev->channels, dev->format);

    frames += (uint64_t)got;
    elapsed = (double)frames / dev->rate;

    if (elapsed < next_at)
      continue;

    /*
     * Smooth against captured time rather than TUNE_INTERVAL, so the needle
     * settles at the same speed whether or not an analysis was skipped.
     */
    aud_tuner_analyse(tuner, elapsed - last_at, &reading);

    if (meter.enabled)
      meter_draw_tuner(&meter, &reading);
    else
      report_change(&reading, &last_midi);

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
