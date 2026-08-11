/* SPDX-License-Identifier: MIT */
#include "cmd/cmd.h"

#include "audio/format.h"
#include "backend/monitor.h"
#include "take/calibrate.h"
#include "take/latency.h"
#include "term/prompt.h"
#include "util/config.h"
#include "util/log.h"
#include "util/path.h"
#include "util/signals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The setting this run exists to fill in. Spelled once, here, because the same
 * string is offered to the user, written to the file, and read back by
 * aud_config_parse().
 */
#define CALIBRATE_KEY "latency_ms"

/* What a single burst is reported as, once its window has closed. */
static void report_burst(aud_calibrate *cal, unsigned index, unsigned repeats)
{
  double ms;
  double match;

  if (aud_calibrate_reading(cal, index, &ms, &match) == 0)
  {
    aud_info(" %u/%u  %6.1f ms   match %.2f", index + 1, repeats, ms, match);
  }
  else
  {
    aud_info(" %u/%u       -      nothing came back", index + 1, repeats);
  }
}

/* Non-zero for an answer that means yes, and for nothing else. */
static int said_yes(const char *answer)
{
  return strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 ||
         strcmp(answer, "yes") == 0 || strcmp(answer, "YES") == 0 ||
         strcmp(answer, "Yes") == 0;
}

/*
 * Whether to stop and ask about keeping the number.
 *
 * The same rule the recorder applies to asking where a take goes, for the same
 * reason: a question nobody is there to answer is a run that appears to have
 * hung, and --quiet asked for errors only, which a question is not.
 */
static int should_ask(const aud_options *opts)
{
  if (opts->prompt == AUD_PROMPT_NEVER)
  {
    return 0;
  }
  if (opts->prompt == AUD_PROMPT_ALWAYS)
  {
    return 1;
  }

  return opts->log_level != AUD_LOG_QUIET && aud_prompt_available();
}

/*
 * Offer to keep the number, and keep it if asked.
 *
 * Only ever at a terminal. A measurement taken from a script leaves the file
 * alone: a run that quietly edited a config file nobody was watching is exactly
 * the kind of thing that is discovered months later, and the number is on
 * stdout either way for anything that wants it.
 */
static void offer_to_save(const aud_options *opts, double ms)
{
  char path[AUD_PATH_MAX];
  char shown[AUD_PATH_MAX];
  char question[AUD_PATH_MAX + 64];
  char answer[AUD_PROMPT_LINE_MAX];
  char value[32];
  char written[AUD_PATH_MAX];

  snprintf(value, sizeof(value), "%.1f", ms);

  if (aud_config_path(path, sizeof(path)) != 0 ||
      aud_path_shorten(shown, sizeof(shown), path) != 0)
  {
    snprintf(shown, sizeof(shown), "the config file");
  }

  if (!should_ask(opts))
  {
    aud_info("put it in %s as '" CALIBRATE_KEY " = %s'", shown, value);
    return;
  }

  snprintf(question, sizeof(question), "write '" CALIBRATE_KEY " = %s' to %s? [y/N]",
           value, shown);

  /* Ctrl+C or Ctrl+D at the question is an answer, and the answer is no */
  if (aud_prompt_line(question, "n", answer, sizeof(answer)) != 0 || !said_yes(answer))
  {
    aud_info("left alone; put it in %s yourself as '" CALIBRATE_KEY " = %s'", shown,
             value);
    return;
  }

  if (aud_config_save(CALIBRATE_KEY, value, written, sizeof(written)) != 0)
  {
    /* aud_config_save() has already said which part of writing it failed */
    return;
  }

  if (aud_path_shorten(shown, sizeof(shown), written) != 0)
  {
    snprintf(shown, sizeof(shown), "%s", written);
  }
  aud_info("wrote " CALIBRATE_KEY " = %s to %s", value, shown);
}

/* What the buffers alone would have guessed, for the line that compares them. */
static double estimate_ms(const aud_device *dev, const aud_monitor_config *mon)
{
  uint64_t frames = aud_latency_estimate(
      dev->period_frames, (unsigned long)mon->period_frames * mon->periods);

  return 1000.0 * (double)frames / dev->rate;
}

static void report_result(const aud_device *dev, const aud_monitor_config *mon,
                          const aud_calibrate_result *result, unsigned xruns,
                          const aud_options *opts)
{
  if (result->verdict != AUD_CALIBRATE_OK)
  {
    aud_error("%s", aud_calibrate_verdict_text(result->verdict));
    if (result->fired > 0 && result->verdict != AUD_CALIBRATE_DROPPED)
    {
      aud_info("the loudest anything got while listening was %.1f dBFS",
               result->peak_dbfs);
    }
    return;
  }

  /*
   * The measurement itself goes to stdout, the way --info and --list write
   * their reports, and everything around it stays on stderr. It is the product
   * of the run rather than a remark about it - so it survives --quiet, and a
   * script can read it without the progress lines landing in the middle.
   */
  printf("round trip:  %.1f ms  (%llu frames at %u Hz)\n", result->ms,
         (unsigned long long)result->frames, dev->rate);
  printf("spread:      %.1f ms  over %u reading(s), weakest match %.2f\n",
         result->spread_ms, result->taken, result->match);
  printf("estimate:    %.1f ms  (what the buffers alone would have guessed)\n",
         estimate_ms(dev, mon));

  /*
   * The spread is the jitter between two clocks that are not the same crystal,
   * which is the part no correction removes. Saying so beats a number that
   * looks more exact than the thing it describes.
   */
  if (result->spread_ms > 2.0)
  {
    aud_warn("the readings disagree by more than a couple of milliseconds, so the "
             "number is that much less certain than it looks");
  }

  if (xruns > 0)
  {
    aud_warn("%u xrun(s) while measuring: frames the capture lost move a reading, "
             "so try a larger --period or more --periods",
             xruns);
  }

  offer_to_save(opts, result->ms);
}

static int calibrate_run(aud_device *dev, const aud_options *opts)
{
  aud_monitor_config mon_cfg;
  aud_monitor *mon = NULL;
  aud_calibrate_config cal_cfg;
  aud_calibrate *cal = NULL;
  aud_calibrate_result result;
  unsigned char *hw = NULL;
  float *captured = NULL;
  float *playback = NULL;
  size_t hw_bytes;
  size_t samples;
  unsigned reported = 0;
  unsigned xruns = 0;
  int rc = -1;

  hw_bytes = aud_device_period_bytes(dev);
  if (hw_bytes == 0)
  {
    aud_error("unsupported capture format");
    return -1;
  }

  samples = (size_t)dev->period_frames * dev->channels;
  hw = malloc(hw_bytes);
  captured = malloc(samples * sizeof(*captured));
  playback = malloc(samples * sizeof(*playback));
  if (hw == NULL || captured == NULL || playback == NULL)
  {
    aud_error("cannot allocate the buffers to measure with");
    goto out;
  }

  aud_monitor_config_defaults(&mon_cfg, dev->rate, dev->channels);
  if (opts->monitor_device != NULL)
  {
    mon_cfg.name = opts->monitor_device;
  }

  /*
   * The one place an output that will not open is fatal. Everywhere else
   * playback is the convenience and the take is the product, so the recording
   * carries on without it - here the output is half of what is being measured,
   * and there is nothing left to do without one.
   */
  mon = aud_monitor_open(&mon_cfg);
  if (mon == NULL)
  {
    aud_error("cannot open '%s' to measure against; there is nothing to measure "
              "without an output",
              mon_cfg.name);
    goto out;
  }

  aud_calibrate_config_defaults(&cal_cfg, dev->rate);
  cal = aud_calibrate_create(&cal_cfg);
  if (cal == NULL)
  {
    aud_error("cannot measure a %u Hz stream", dev->rate);
    goto out;
  }

  aud_info("measuring the round trip out of '%s' and back into '%s': %u Hz, %u "
           "burst(s)",
           mon_cfg.name, dev->name, dev->rate, cal_cfg.repeats);
  aud_info("connect the output to the input, and play nothing until it is done");

  while (!aud_signals_stop_requested())
  {
    long got = aud_device_read(dev, hw, dev->period_frames, &xruns);
    int done;

    if (got < 0)
    {
      goto out;
    }
    if (got == 0)
    {
      continue;
    }

    aud_format_to_float(captured, hw, (size_t)got, dev->channels, dev->format);

    /*
     * One call for both directions: what came back over this period, and what
     * goes out over it. The round trip is the distance between those two on one
     * clock, so nothing else may advance it. See calibrate.h.
     */
    done = aud_calibrate_step(cal, captured, dev->channels, playback, dev->channels,
                              (size_t)got);

    if (aud_monitor_write(mon, playback, (size_t)got, 1.0f) != 0)
    {
      aud_error("the output failed part way through the measurement");
      goto out;
    }
    aud_calibrate_note_dropped(cal, aud_monitor_dropped(mon));

    while (reported < cal_cfg.repeats && aud_calibrate_ready(cal, reported))
    {
      report_burst(cal, reported, cal_cfg.repeats);
      reported++;
    }

    if (done)
    {
      break;
    }
  }

  aud_calibrate_analyse(cal, &result);
  report_result(dev, &mon_cfg, &result, xruns, opts);
  rc = result.verdict == AUD_CALIBRATE_OK ? 0 : -1;

out:
  aud_calibrate_destroy(cal);
  aud_monitor_close(mon);
  free(playback);
  free(captured);
  free(hw);
  return rc;
}

int aud_cmd_calibrate(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
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

  rc = calibrate_run(&dev, opts);
  aud_device_close(&dev);

  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
