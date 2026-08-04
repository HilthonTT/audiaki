/* SPDX-License-Identifier: MIT */
#include "recorder.h"

#include "format.h"
#include "log.h"
#include "meter.h"
#include "wav.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* how often the meter is redrawn, in seconds of captured audio */
#define METER_INTERVAL 0.05

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_stop = 1;
}

int aud_recorder_install_signals(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  /*
   * Deliberately no SA_RESTART: the blocking read must return with EINTR so
   * the loop notices the stop request without waiting for another period.
   */
  if (sigaction(SIGINT, &sa, NULL) != 0)
    return -1;
  if (sigaction(SIGTERM, &sa, NULL) != 0)
    return -1;
  return 0;
}

int aud_recorder_stop_requested(void)
{
  return g_stop != 0;
}

int aud_recorder_run(aud_device *dev, const aud_recorder_options *opts,
                     aud_recorder_stats *stats)
{
  wav_writer wav;
  aud_meter meter;
  unsigned char *hw_buf = NULL;
  unsigned char *out_buf = NULL;
  size_t hw_buf_bytes;
  size_t out_buf_bytes;
  uint64_t frames_written = 0;
  uint64_t limit_frames = 0;
  unsigned xruns = 0;
  double next_meter_at = 0.0;
  int repack = aud_format_needs_repack(dev->format);
  unsigned hw_bytes = aud_format_hw_bytes(dev->format);
  unsigned wav_bytes = aud_format_wav_bytes(dev->format);
  int rc = -1;

  if (stats != NULL)
    memset(stats, 0, sizeof(*stats));

  if (hw_bytes == 0 || wav_bytes == 0)
  {
    aud_error("unsupported capture format");
    return -1;
  }

  if (opts->duration > 0.0)
  {
    limit_frames = (uint64_t)(opts->duration * (double)dev->rate + 0.5);
    if (limit_frames == 0)
      limit_frames = 1;
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

  if (wav_open(&wav, opts->output_path, dev->rate, (uint16_t)dev->channels,
               (uint16_t)aud_format_wav_bits(dev->format), opts->overwrite) != 0)
  {
    if (errno == EEXIST)
      aud_error("%s already exists (pass --force to overwrite)", opts->output_path);
    else
      aud_perror("cannot create %s", opts->output_path);
    goto out;
  }

  aud_info("recording %s: %u Hz, %u ch, %s -> %u-bit WAV", opts->output_path, dev->rate,
           dev->channels, aud_format_name(dev->format), aud_format_wav_bits(dev->format));
  aud_debug("period %lu frames (%.1f ms), buffer %lu frames", dev->period_frames,
            1000.0 * (double)dev->period_frames / dev->rate, dev->buffer_frames);
  if (opts->duration > 0.0)
    aud_info("stopping after %.2f s (Ctrl+C stops early)", opts->duration);
  else
    aud_info("press Ctrl+C to stop");

  while (!g_stop)
  {
    unsigned long want = dev->period_frames;
    long got;
    size_t samples;
    size_t nbytes;
    double elapsed;

    /* read only what is still missing so -t lands on an exact frame count */
    if (limit_frames != 0 && frames_written + want > limit_frames)
      want = (unsigned long)(limit_frames - frames_written);

    got = aud_device_read(dev, hw_buf, want, &xruns);
    if (got < 0)
      goto finish;
    if (got == 0)
      continue;

    samples = (size_t)got * dev->channels;
    nbytes = samples * wav_bytes;

    if (wav_would_overflow(&wav, nbytes))
    {
      meter_clear(&meter);
      aud_warn("reached the 4 GB WAV size limit, stopping");
      break;
    }

    if (repack)
      aud_format_repack(out_buf, hw_buf, samples, dev->format);

    if (wav_write(&wav, out_buf, nbytes) != 0)
    {
      meter_clear(&meter);
      aud_perror("cannot write to %s", opts->output_path);
      goto finish;
    }

    frames_written += (uint64_t)got;
    elapsed = (double)frames_written / dev->rate;

    if (elapsed >= next_meter_at)
    {
      meter_draw(&meter, aud_format_peak(hw_buf, (size_t)got, dev->channels, dev->format),
                 elapsed, xruns);
      next_meter_at = elapsed + METER_INTERVAL;
    }

    if (limit_frames != 0 && frames_written >= limit_frames)
      break;
  }

  rc = 0;

finish:
  meter_clear(&meter);
  snd_pcm_drop(dev->pcm);

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
      aud_warn("input clipped - lower the level on the device and record again");
  }

  if (stats != NULL)
  {
    stats->frames = frames_written;
    stats->bytes = frames_written * dev->channels * wav_bytes;
    stats->xruns = xruns;
    stats->clipped = meter_clipped(&meter);
    stats->interrupted = g_stop != 0;
  }

out:
  if (repack)
    free(out_buf);
  free(hw_buf);
  return rc;
}
