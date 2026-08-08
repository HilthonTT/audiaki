/* SPDX-License-Identifier: MIT */
#include "recorder.h"

#include "format.h"
#include "log.h"
#include "meter.h"
#include "monitor.h"
#include "preroll.h"
#include "signals.h"
#include "spectrum.h"
#include "wav.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* how often the meter is redrawn, in seconds of captured audio */
#define METER_INTERVAL 0.05

int aud_recorder_install_signals(void)
{
  return aud_signals_install_stop();
}

/*
 * The optional playback of what is being captured. `mon` is NULL whenever
 * monitoring is off, which is both the default and where an output that has
 * failed ends up: nothing here is allowed to interrupt the take.
 */
typedef struct
{
  aud_monitor *mon;
  float *buf; /* period_frames * channels, interleaved */
  float gain;
  unsigned long dropped;
} recorder_monitor;

/*
 * Open the output to monitor through, if one was asked for. Never fails in a
 * way the caller has to handle: an output that will not open costs the
 * monitoring and leaves the recording alone, which is the trade monitor.h
 * expects callers to make.
 */
static void monitor_start(recorder_monitor *rm, const aud_device *dev,
                          const aud_recorder_options *opts)
{
  aud_monitor_config cfg;

  rm->gain = opts->monitor_gain;

  if (!opts->monitor)
  {
    return;
  }

  rm->buf = malloc((size_t)dev->period_frames * dev->channels * sizeof(*rm->buf));
  if (rm->buf == NULL)
  {
    aud_warn("cannot allocate a monitoring buffer, recording without monitoring");
    return;
  }

  aud_monitor_config_defaults(&cfg, dev->rate, dev->channels);
  if (opts->monitor_device != NULL)
  {
    cfg.name = opts->monitor_device;
  }

  rm->mon = aud_monitor_open(&cfg);
  if (rm->mon == NULL)
  {
    /* the backend has already said which part of opening the output failed */
    aud_warn("recording without monitoring");
    free(rm->buf);
    rm->buf = NULL;
    return;
  }

  /*
   * A warning rather than a remark: the first thing anyone does is try this on
   * a laptop, where the default capture is the built-in microphone and the
   * default output is the speaker beside it. That is a feedback loop, and it
   * reaches full scale in a fraction of a second.
   */
  aud_warn("monitoring through %s - use headphones; a microphone played through "
           "speakers will feed back",
           cfg.name);
}

/* Idempotent, so the cleanup path can run it whichever way the take ended. */
static void monitor_stop(recorder_monitor *rm)
{
  if (rm->mon != NULL)
  {
    rm->dropped = aud_monitor_dropped(rm->mon);
    aud_monitor_close(rm->mon);
    rm->mon = NULL;
  }

  free(rm->buf);
  rm->buf = NULL;
}

/*
 * Hand a captured period to the output. Decodes hw_buf rather than the
 * repacked copy for the same reason the analysis does: that is what the device
 * delivered, and the two may be the same buffer anyway.
 */
static void monitor_feed(recorder_monitor *rm, const unsigned char *hw_buf, size_t frames,
                         const aud_device *dev)
{
  if (rm->mon == NULL)
  {
    return;
  }

  aud_format_to_float(rm->buf, hw_buf, frames, dev->channels, dev->format);

  if (aud_monitor_write(rm->mon, rm->buf, frames, rm->gain) != 0)
  {
    rm->dropped = aud_monitor_dropped(rm->mon);
    aud_monitor_close(rm->mon);
    rm->mon = NULL;
    aud_warn("monitoring stopped: the output failed (the take is still recording)");
  }
}

/*
 * Non-zero once the take should begin. Polled between periods rather than read
 * on a thread of its own; a period is a few milliseconds, which is well inside
 * what a keypress needs to feel immediate.
 *
 * End of input counts as a start: a pipe or /dev/null on stdin means no one is
 * ever going to press anything, and waiting for a key that cannot arrive is
 * worse than recording the take that was asked for.
 */
static int start_requested(void)
{
  struct pollfd pfd;
  char buf[64];
  ssize_t got;

  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  pfd.revents = 0;

  if (poll(&pfd, 1, 0) <= 0)
  {
    return 0;
  }
  if ((pfd.revents & POLLIN) == 0)
  {
    return 1;
  }

  got = read(STDIN_FILENO, buf, sizeof(buf));
  if (got <= 0)
  {
    return 1;
  }

  for (ssize_t i = 0; i < got; i++)
  {
    if (buf[i] == '\n')
    {
      return 1;
    }
  }
  return 0;
}

/*
 * Capture into `pre` without writing anything, until Enter is pressed. Returns
 * 0 to start recording, 1 when the wait was interrupted and no take should be
 * created at all, or -1 if the device failed.
 */
static int arm_and_wait(aud_device *dev, unsigned char *hw_buf, aud_preroll *pre,
                        aud_meter *meter, aud_spectrum *spectrum, size_t bands,
                        recorder_monitor *rm, unsigned *xruns)
{
  double next_meter_at = 0.0;
  double last_drawn_at = 0.0;
  uint64_t seen = 0;

  aud_info("armed: holding the last %.1f s - press Enter to record, Ctrl+C to quit",
           (double)aud_preroll_capacity(pre) / dev->rate);
  meter_set_armed(meter, 1);

  while (!aud_signals_stop_requested())
  {
    long got;
    double captured;

    if (start_requested())
    {
      meter_set_armed(meter, 0);
      meter_clear(meter);
      return 0;
    }

    got = aud_device_read(dev, hw_buf, dev->period_frames, xruns);
    if (got < 0)
    {
      return -1;
    }
    if (got == 0)
    {
      continue;
    }

    aud_preroll_push(pre, hw_buf, (size_t)got);

    if (spectrum != NULL)
    {
      aud_spectrum_push_pcm(spectrum, hw_buf, (size_t)got, dev->channels, dev->format);
    }

    /* audible while armed too: the level is set before the take, not during it */
    monitor_feed(rm, hw_buf, (size_t)got, dev);

    seen += (uint64_t)got;
    captured = (double)seen / dev->rate;

    if (captured >= next_meter_at)
    {
      double peak = aud_format_peak(hw_buf, (size_t)got, dev->channels, dev->format);
      /* what is held, not how long the wait has been: a take would start here */
      double held = (double)aud_preroll_filled(pre) / dev->rate;

      if (spectrum != NULL)
      {
        const float *values = aud_spectrum_analyse(spectrum, captured - last_drawn_at);
        meter_draw_spectrum(meter, values, bands, peak, held, *xruns);
      }
      else
      {
        meter_draw(meter, peak, held, *xruns);
      }

      last_drawn_at = captured;
      next_meter_at = captured + METER_INTERVAL;
    }
  }

  meter_set_armed(meter, 0);
  meter_clear(meter);
  return 1;
}

/*
 * Write everything `pre` holds to the take, oldest first, in period sized
 * pieces so the repack has somewhere to land.
 */
static int flush_preroll(wav_writer *wav, const aud_preroll *pre, const aud_device *dev,
                         unsigned char *out_buf, int repack, const char *path,
                         uint64_t *frames_written)
{
  aud_preroll_segment seg[2];
  unsigned segments = aud_preroll_segments(pre, seg);
  size_t frame_bytes = (size_t)dev->channels * aud_format_hw_bytes(dev->format);
  unsigned wav_bytes = aud_format_wav_bytes(dev->format);

  for (unsigned s = 0; s < segments; s++)
  {
    size_t done = 0;

    while (done < seg[s].frames)
    {
      const unsigned char *src = seg[s].data + done * frame_bytes;
      size_t frames = seg[s].frames - done;
      size_t nbytes;

      if (frames > dev->period_frames)
      {
        frames = dev->period_frames;
      }
      nbytes = frames * dev->channels * wav_bytes;

      if (wav_would_overflow(wav, nbytes))
      {
        aud_warn("the pre-roll does not fit in a WAV file, dropping the rest");
        return 0;
      }

      if (repack)
      {
        aud_format_repack(out_buf, src, frames * dev->channels, dev->format);
        src = out_buf;
      }

      if (wav_write(wav, src, nbytes) != 0)
      {
        aud_perror("cannot write to %s", path);
        return -1;
      }

      *frames_written += frames;
      done += frames;
    }
  }

  return 0;
}

int aud_recorder_run(aud_device *dev, const aud_recorder_options *opts,
                     aud_recorder_stats *stats)
{
  wav_writer wav;
  aud_meter meter;
  aud_spectrum *spectrum = NULL;
  recorder_monitor rm;
  aud_preroll pre;
  size_t bands = 0;
  unsigned char *hw_buf = NULL;
  unsigned char *out_buf = NULL;
  size_t hw_buf_bytes;
  size_t out_buf_bytes;
  uint64_t frames_written = 0;
  uint64_t preroll_frames = 0;
  uint64_t recorded = 0; /* frames captured since the take started */
  uint64_t limit_frames = 0;
  unsigned xruns = 0;
  int cancelled = 0;
  double next_meter_at = 0.0;
  double last_drawn_at = 0.0;
  int repack = aud_format_needs_repack(dev->format);
  unsigned hw_bytes = aud_format_hw_bytes(dev->format);
  unsigned wav_bytes = aud_format_wav_bytes(dev->format);
  int rc = -1;

  memset(&pre, 0, sizeof(pre));
  memset(&rm, 0, sizeof(rm));

  if (stats != NULL)
  {
    memset(stats, 0, sizeof(*stats));
  }

  if (hw_bytes == 0 || wav_bytes == 0)
  {
    aud_error("unsupported capture format");
    return -1;
  }

  if (opts->duration > 0.0)
  {
    limit_frames = (uint64_t)(opts->duration * (double)dev->rate + 0.5);
    if (limit_frames == 0)
    {
      limit_frames = 1;
    }
  }

  hw_buf_bytes = (size_t)dev->period_frames * dev->channels * hw_bytes;
  out_buf_bytes = (size_t)dev->period_frames * dev->channels * wav_bytes;

  hw_buf = malloc(hw_buf_bytes);
  if (hw_buf == NULL)
  {
    aud_error("cannot allocate a %zu byte capture buffer", hw_buf_bytes);
    return -1;
  }

  out_buf = repack ? malloc(out_buf_bytes) : hw_buf;
  if (out_buf == NULL)
  {
    aud_error("cannot allocate a %zu byte output buffer", out_buf_bytes);
    free(hw_buf);
    return -1;
  }

  /* initialised before any early exit so the cleanup path is unconditional */
  meter_init(&meter, opts->show_meter);

  if (opts->show_spectrum && opts->show_meter)
  {
    aud_spectrum_config spec_cfg;

    bands = meter_fit_bands(&meter);
    if (bands > 0)
    {
      aud_spectrum_config_defaults(&spec_cfg, dev->rate, bands);
      spectrum = aud_spectrum_create(&spec_cfg);
      if (spectrum == NULL)
      {
        /* not worth aborting a take over: fall back to the peak bar */
        aud_warn("cannot set up the spectrum display, showing the peak meter");
        bands = 0;
      }
    }
  }

  monitor_start(&rm, dev, opts);

  if (opts->preroll > 0.0)
  {
    size_t frames = aud_preroll_frames_for(opts->preroll, dev->rate);
    int armed;

    /* a ring shorter than a period would be emptied by the first read */
    if (frames < dev->period_frames)
    {
      frames = dev->period_frames;
    }

    if (aud_preroll_init(&pre, frames, (size_t)dev->channels * hw_bytes) != 0)
    {
      aud_perror("cannot hold %.1f s of pre-roll", opts->preroll);
      goto out;
    }
    aud_debug("pre-roll: %zu frames, %.1f MiB", frames,
              (double)(frames * dev->channels * hw_bytes) / (1024.0 * 1024.0));

    armed = arm_and_wait(dev, hw_buf, &pre, &meter, spectrum, bands, &rm, &xruns);
    if (armed < 0)
    {
      goto out;
    }
    if (armed > 0)
    {
      aud_info("nothing recorded");
      cancelled = 1;
      rc = 0;
      goto out;
    }

    /* the summary afterwards is about the take, not about setting the level */
    xruns = 0;
    meter_reset_peaks(&meter);
  }

  if (wav_open(&wav, opts->output_path, dev->rate, (uint16_t)dev->channels,
               (uint16_t)aud_format_wav_bits(dev->format), opts->overwrite) != 0)
  {
    if (errno == EEXIST)
    {
      aud_error("%s already exists (pass --force to overwrite)", opts->output_path);
    }
    else
    {
      aud_perror("cannot create %s", opts->output_path);
    }
    goto out;
  }

  aud_info("recording %s: %u Hz, %u ch, %s -> %u-bit WAV", opts->output_path, dev->rate,
           dev->channels, aud_format_name(dev->format), aud_format_wav_bits(dev->format));
  aud_debug("period %lu frames (%.1f ms), buffer %lu frames", dev->period_frames,
            1000.0 * (double)dev->period_frames / dev->rate, dev->buffer_frames);
  if (opts->duration > 0.0)
  {
    aud_info("stopping after %.2f s (Ctrl+C stops early)", opts->duration);
  }
  else
  {
    aud_info("press Ctrl+C to stop");
  }

  if (aud_preroll_filled(&pre) > 0)
  {
    if (flush_preroll(&wav, &pre, dev, out_buf, repack, opts->output_path,
                      &frames_written) != 0)
    {
      goto finish;
    }
    preroll_frames = frames_written;
    aud_info("prepended %.1f s of pre-roll", (double)preroll_frames / dev->rate);
  }

  while (!aud_signals_stop_requested())
  {
    unsigned long want = dev->period_frames;
    long got;
    size_t samples;
    size_t nbytes;
    double elapsed;

    /*
     * Read only what is still missing so -t lands on an exact frame count.
     * Against `recorded`, not the file: --duration is how long to record for,
     * and pre-roll is time that had already passed when it started.
     */
    if (limit_frames != 0 && recorded + want > limit_frames)
    {
      want = (unsigned long)(limit_frames - recorded);
    }

    got = aud_device_read(dev, hw_buf, want, &xruns);
    if (got < 0)
    {
      goto finish;
    }
    if (got == 0)
    {
      continue;
    }

    samples = (size_t)got * dev->channels;
    nbytes = samples * wav_bytes;

    if (wav_would_overflow(&wav, nbytes))
    {
      meter_clear(&meter);
      aud_warn("reached the 4 GB WAV size limit, stopping");
      break;
    }

    if (repack)
    {
      aud_format_repack(out_buf, hw_buf, samples, dev->format);
    }

    if (wav_write(&wav, out_buf, nbytes) != 0)
    {
      meter_clear(&meter);
      aud_perror("cannot write to %s", opts->output_path);
      goto finish;
    }

    /*
     * Analyse the captured period, not the repacked copy: hw_buf is what the
     * device delivered, and out_buf may alias it anyway.
     */
    if (spectrum != NULL)
    {
      aud_spectrum_push_pcm(spectrum, hw_buf, (size_t)got, dev->channels, dev->format);
    }

    monitor_feed(&rm, hw_buf, (size_t)got, dev);

    frames_written += (uint64_t)got;
    recorded += (uint64_t)got;
    elapsed = (double)frames_written / dev->rate;

    if (elapsed >= next_meter_at)
    {
      double peak = aud_format_peak(hw_buf, (size_t)got, dev->channels, dev->format);

      if (spectrum != NULL)
      {
        /*
         * Smooth against captured time rather than METER_INTERVAL, so the bars
         * decay at the same rate whether or not a redraw was skipped.
         */
        const float *values = aud_spectrum_analyse(spectrum, elapsed - last_drawn_at);
        meter_draw_spectrum(&meter, values, bands, peak, elapsed, xruns);
      }
      else
      {
        meter_draw(&meter, peak, elapsed, xruns);
      }

      last_drawn_at = elapsed;
      next_meter_at = elapsed + METER_INTERVAL;
    }

    if (limit_frames != 0 && recorded >= limit_frames)
    {
      break;
    }
  }

  rc = 0;

finish:
  meter_clear(&meter);
  aud_device_drop(dev);

  if (wav_close(&wav) != 0)
  {
    aud_perror("cannot finalise %s", opts->output_path);
    rc = -1;
  }

  if (rc == 0)
  {
    aud_info("wrote %s: %.2f s, %.1f MiB, %u xrun(s)%s", opts->output_path,
             (double)frames_written / dev->rate,
             (double)(frames_written * dev->channels * wav_bytes) / (1024.0 * 1024.0),
             xruns, meter_clipped(&meter) ? ", clipping detected" : "");
    if (meter_clipped(&meter))
    {
      aud_warn("input clipped - lower the level on the device and record again");
    }
  }

out:
  monitor_stop(&rm);
  if (rm.dropped > 0)
  {
    /*
     * Expected rather than wrong: capture and playback do not share a clock, so
     * the monitor skips instead of drifting further behind. Worth having under
     * -v when what was heard did not sound like what was written.
     */
    aud_debug("the monitor dropped %lu frame(s) keeping up with the input", rm.dropped);
  }

  aud_spectrum_destroy(spectrum);
  aud_preroll_free(&pre);
  if (repack)
  {
    free(out_buf);
  }
  free(hw_buf);

  if (stats != NULL)
  {
    stats->frames = frames_written;
    stats->bytes = frames_written * dev->channels * wav_bytes;
    stats->preroll_frames = preroll_frames;
    stats->xruns = xruns;
    stats->monitor_dropped = rm.dropped;
    stats->clipped = meter_clipped(&meter);
    stats->interrupted = aud_signals_stop_requested();
    stats->cancelled = cancelled;
  }

  return rc;
}
