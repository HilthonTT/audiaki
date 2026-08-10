/* SPDX-License-Identifier: MIT */
#include "cmd/cmd.h"

#include "audio/format.h"
#include "audio/spectrum.h"
#include "cmd/playback.h"
#include "media/wav.h"
#include "take/preroll.h"
#include "take/take.h"
#include "term/meter.h"
#include "term/prompt.h"
#include "util/log.h"
#include "util/parse.h"
#include "util/path.h"
#include "util/signals.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * What a take is made from. Filled in from aud_options by aud_cmd_record() at
 * the bottom of this file; kept as a struct of its own because the capture loop
 * is long enough that reading it against a named field beats reading it against
 * an option letter.
 */
typedef struct
{
  const char *output_path;
  /*
   * Which single capture channel to write, counting from 1, or 0 for all of
   * them. The device is still opened with whatever channel count was asked
   * for; this is only about what reaches the file.
   */
  unsigned channel;
  double duration;   /* seconds; 0 records until interrupted */
  int overwrite;     /* allow replacing an existing file */
  int show_meter;    /* draw a live display at all */
  int show_spectrum; /* make that display spectrum bars, not the peak bar */
  /*
   * Seconds to hold before the take starts; 0 records as soon as the device is
   * open. Anything else arms instead: nothing is written until Enter, and the
   * take then opens with the seconds leading up to it.
   */
  double preroll;
  /*
   * Play the input back through an output while it is being captured, and/or
   * run a metronome into the same one - see playback.h, which owns both and the
   * rule that neither may end a take. They reach the headphones and not the
   * file: whatever these are set to, the take is written from the samples the
   * device delivered.
   */
  int monitor;
  const char *monitor_device;
  float monitor_gain;
  double click_bpm;
  unsigned click_beats;
  unsigned click_subdiv;
  float click_gain;
  /*
   * Stamp the take with what made it, when, and from what device - see meta.h.
   * On by default; clearing it writes the plain 44 byte header instead, for
   * anyone whose tools want nothing between the fmt and data chunks.
   *
   * `note` is free text to carry along with it, or NULL for none.
   */
  int metadata;
  const char *note;
} aud_recorder_options;

typedef struct
{
  uint64_t frames;
  uint64_t bytes;
  uint64_t preroll_frames; /* of `frames`, how many came from before the start */
  unsigned xruns;
  unsigned long monitor_dropped; /* frames the monitor output could not keep up with */
  int clipped;
  int interrupted; /* stopped by SIGINT/SIGTERM rather than reaching the end */
  int cancelled;   /* interrupted while armed, so no file was created */
} aud_recorder_stats;

/*
 * Record from `dev` into opts->output_path. Returns 0 on success and -1 on
 * failure, after reporting the reason through log.h. `stats` may be NULL.
 */
static int recorder_run(aud_device *dev, const aud_recorder_options *opts,
                        aud_recorder_stats *stats);

/* how often the meter is redrawn, in seconds of captured audio */
#define METER_INTERVAL 0.05

/*
 * How a captured period becomes what the file holds.
 *
 * Two things can sit between the device and the WAV: --channel drops every
 * channel but one, and some capture formats need repacking. The capture loop
 * and the pre-roll flush both have to apply them, in that order, so it is
 * worked out once here rather than twice down there.
 */
typedef struct
{
  aud_format format;
  unsigned in_channels;  /* what the device delivers */
  unsigned out_channels; /* what goes in the file; 1 once reduced to one channel */
  unsigned pick;         /* the channel to keep, 0-based; read only when picking */
  int picking;
  int mixing; /* every channel averaged into one; never set with `picking` */
  int repack;
  unsigned wav_bytes;
  unsigned char *pick_buf; /* one period of the one channel out, capture layout */
  unsigned char *out_buf;  /* one period in the WAV layout; NULL without a repack */
} recorder_shape;

/*
 * Reduce one captured period to the bytes that go in the file. Returns the
 * buffer to write and sets *nbytes.
 *
 * `analysis` may be NULL; when it is not, it is set to the frames the meter and
 * the spectrum should read. With a channel picked that is the picked channel
 * alone, because metering a channel that is not being recorded is worse than
 * not metering at all - it is the wrong answer rather than no answer.
 */
static const unsigned char *shape_frames(const recorder_shape *sh,
                                         const unsigned char *hw, size_t frames,
                                         size_t *nbytes, const unsigned char **analysis)
{
  const unsigned char *src = hw;

  if (sh->picking)
  {
    aud_format_pick_channel(sh->pick_buf, hw, frames, sh->in_channels, sh->pick,
                            sh->format);
    src = sh->pick_buf;
  }
  else if (sh->mixing)
  {
    aud_format_mix_channels(sh->pick_buf, hw, frames, sh->in_channels, sh->format);
    src = sh->pick_buf;
  }

  if (analysis != NULL)
  {
    *analysis = src;
  }
  *nbytes = frames * sh->out_channels * sh->wav_bytes;

  if (sh->repack)
  {
    aud_format_repack(sh->out_buf, src, frames * sh->out_channels, sh->format);
    return sh->out_buf;
  }

  return src;
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
static int arm_and_wait(aud_device *dev, const recorder_shape *shape,
                        unsigned char *hw_buf, aud_preroll *pre, aud_meter *meter,
                        aud_spectrum *spectrum, size_t bands, aud_playback *pb,
                        unsigned *xruns)
{
  double next_meter_at = 0.0;
  double last_drawn_at = 0.0;
  uint64_t seen = 0;

  aud_info("armed: holding the last %.1f s - press Enter to record, Ctrl+C to quit",
           (double)aud_preroll_capacity(pre) / dev->rate);
  meter_set_armed(meter, 1);

  while (!aud_signals_stop_requested())
  {
    const unsigned char *analysis;
    long got;
    size_t nbytes;
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

    /*
     * Shaped only to be looked at - what it returns is dropped, and the ring
     * above keeps every channel. Setting the level is the whole point of being
     * armed, so the meter has to read the channel the take will be made of.
     */
    (void)shape_frames(shape, hw_buf, (size_t)got, &nbytes, &analysis);

    if (spectrum != NULL)
    {
      aud_spectrum_push_pcm(spectrum, analysis, (size_t)got, shape->out_channels,
                            dev->format);
    }

    /*
     * Audible while armed too: the level is set before the take, not during
     * it, and a metronome that only started at the keypress would be a count-in
     * nobody got to hear.
     */
    aud_playback_feed(pb, hw_buf, (size_t)got, dev);

    seen += (uint64_t)got;
    captured = (double)seen / dev->rate;

    if (captured >= next_meter_at)
    {
      double peak =
          aud_format_peak(analysis, (size_t)got, shape->out_channels, dev->format);
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
                         const recorder_shape *shape, const char *path,
                         uint64_t *frames_written)
{
  aud_preroll_segment seg[2];
  unsigned segments = aud_preroll_segments(pre, seg);
  size_t frame_bytes = (size_t)dev->channels * aud_format_hw_bytes(dev->format);

  for (unsigned s = 0; s < segments; s++)
  {
    size_t done = 0;

    while (done < seg[s].frames)
    {
      const unsigned char *held = seg[s].data + done * frame_bytes;
      const unsigned char *payload;
      size_t frames = seg[s].frames - done;
      size_t nbytes;

      if (frames > dev->period_frames)
      {
        frames = dev->period_frames;
      }

      /*
       * The ring holds whole frames as the device delivered them, so the
       * pre-roll is picked and repacked here on the way out, exactly as a live
       * period is. Nothing is decided when it goes in, which is what keeps it
       * bit for bit.
       */
      payload = shape_frames(shape, held, frames, &nbytes, NULL);

      if (wav_would_overflow(wav, nbytes))
      {
        aud_warn("the pre-roll does not fit in a WAV file, dropping the rest");
        return 0;
      }

      if (wav_write(wav, payload, nbytes) != 0)
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

static int recorder_run(aud_device *dev, const aud_recorder_options *opts,
                        aud_recorder_stats *stats)
{
  wav_writer wav;
  aud_meter meter;
  aud_meta meta;
  aud_spectrum *spectrum = NULL;
  aud_playback pb;
  aud_preroll pre;
  recorder_shape shape;
  size_t bands = 0;
  unsigned char *hw_buf = NULL;
  unsigned char *pick_buf = NULL;
  unsigned char *out_buf = NULL;
  size_t hw_buf_bytes;
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

  if (stats != NULL)
  {
    memset(stats, 0, sizeof(*stats));
  }

  if (hw_bytes == 0 || wav_bytes == 0)
  {
    aud_error("unsupported capture format");
    return -1;
  }

  memset(&shape, 0, sizeof(shape));
  shape.format = dev->format;
  shape.in_channels = dev->channels;
  shape.out_channels = dev->channels;
  shape.repack = repack;
  shape.wav_bytes = wav_bytes;

  /*
   * Checked against what the device negotiated rather than what was asked for:
   * a card that would not give four channels and settled on two makes
   * '--channel 3' a mistake worth stopping for, and the parser could not have
   * known that.
   */
  if (opts->channel == AUD_CHANNEL_MIX)
  {
    shape.mixing = 1;
    shape.out_channels = 1;
  }
  else if (opts->channel > 0)
  {
    if (opts->channel > dev->channels)
    {
      aud_error("--channel %u, but %s gave %u channel(s)", opts->channel, dev->name,
                dev->channels);
      aud_info("channels are numbered from 1; --probe shows what the device offers, "
               "and --channel mix takes all of them at once");
      return -1;
    }
    shape.picking = 1;
    shape.pick = opts->channel - 1u;
    shape.out_channels = 1;
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

  hw_buf = malloc(hw_buf_bytes);
  if (hw_buf == NULL)
  {
    aud_error("cannot allocate a %zu byte capture buffer", hw_buf_bytes);
    return -1;
  }

  if (shape.picking || shape.mixing)
  {
    size_t pick_bytes = (size_t)dev->period_frames * hw_bytes;

    pick_buf = malloc(pick_bytes);
    if (pick_buf == NULL)
    {
      aud_error("cannot allocate a %zu byte channel buffer", pick_bytes);
      free(hw_buf);
      return -1;
    }
  }

  /*
   * Only a repack needs a second buffer. Without one the frames go to the
   * writer straight out of hw_buf, or out of pick_buf when a channel has been
   * picked, and neither is copied for the sake of it.
   */
  if (repack)
  {
    size_t out_buf_bytes = (size_t)dev->period_frames * shape.out_channels * wav_bytes;

    out_buf = malloc(out_buf_bytes);
    if (out_buf == NULL)
    {
      aud_error("cannot allocate a %zu byte output buffer", out_buf_bytes);
      free(pick_buf);
      free(hw_buf);
      return -1;
    }
  }

  shape.pick_buf = pick_buf;
  shape.out_buf = out_buf;

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

  {
    aud_playback_config pb_cfg;

    pb_cfg.input = opts->monitor;
    pb_cfg.device = opts->monitor_device;
    pb_cfg.gain = opts->monitor_gain;
    pb_cfg.click_bpm = opts->click_bpm;
    pb_cfg.click_beats = opts->click_beats;
    pb_cfg.click_subdiv = opts->click_subdiv;
    pb_cfg.click_gain = opts->click_gain;
    aud_playback_start(&pb, dev, &pb_cfg);
  }

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

    armed = arm_and_wait(dev, &shape, hw_buf, &pre, &meter, spectrum, bands, &pb, &xruns);
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

  /*
   * Stamped when the take starts rather than when it ends, so the time in the
   * file is the time the first frame was captured. With --preroll that is the
   * moment Enter was pressed; the seconds before it are older than their own
   * timestamp by exactly the pre-roll, which is the only reading of it that
   * does not need the pre-roll length to make sense of.
   */
  aud_meta_defaults(&meta);
  meta.device = dev->name;
  meta.note = opts->note;
  meta.rate = dev->rate;
  meta.channels = shape.out_channels;
  meta.bits = aud_format_wav_bits(dev->format);
  /*
   * The click is heard and not recorded, so the file has no trace of it in the
   * audio - which is exactly why the tempo has to be written down. A take is
   * otherwise a performance to a grid nothing remembers.
   */
  meta.click_bpm = opts->click_bpm;
  meta.click_beats = opts->click_beats;
  meta.click_subdiv = opts->click_subdiv;
  aud_meta_stamp_now(&meta, dev->rate);

  if (wav_open_meta(&wav, opts->output_path, dev->rate, (uint16_t)shape.out_channels,
                    (uint16_t)aud_format_wav_bits(dev->format), opts->overwrite,
                    opts->metadata ? &meta : NULL) != 0)
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

  if (shape.picking)
  {
    aud_info("recording %s: %u Hz, channel %u of %u as mono, %s -> %u-bit WAV",
             opts->output_path, dev->rate, opts->channel, dev->channels,
             aud_format_name(dev->format), aud_format_wav_bits(dev->format));
  }
  else if (shape.mixing)
  {
    aud_info("recording %s: %u Hz, %u channels mixed to mono, %s -> %u-bit WAV",
             opts->output_path, dev->rate, dev->channels, aud_format_name(dev->format),
             aud_format_wav_bits(dev->format));
  }
  else
  {
    aud_info("recording %s: %u Hz, %u ch, %s -> %u-bit WAV", opts->output_path, dev->rate,
             dev->channels, aud_format_name(dev->format),
             aud_format_wav_bits(dev->format));
  }
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
    if (flush_preroll(&wav, &pre, dev, &shape, opts->output_path, &frames_written) != 0)
    {
      goto finish;
    }
    preroll_frames = frames_written;
    aud_info("prepended %.1f s of pre-roll", (double)preroll_frames / dev->rate);
  }

  while (!aud_signals_stop_requested())
  {
    unsigned long want = dev->period_frames;
    const unsigned char *payload;
    const unsigned char *analysis;
    long got;
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

    payload = shape_frames(&shape, hw_buf, (size_t)got, &nbytes, &analysis);

    if (wav_would_overflow(&wav, nbytes))
    {
      meter_clear(&meter);
      aud_warn("reached the 4 GB WAV size limit, stopping");
      break;
    }

    if (wav_write(&wav, payload, nbytes) != 0)
    {
      meter_clear(&meter);
      aud_perror("cannot write to %s", opts->output_path);
      goto finish;
    }

    /*
     * Analyse what is being written rather than the repacked bytes: `analysis`
     * is still in the capture layout, and it is the picked channel when there
     * is one, so the meter answers for the take rather than for the device.
     */
    if (spectrum != NULL)
    {
      aud_spectrum_push_pcm(spectrum, analysis, (size_t)got, shape.out_channels,
                            dev->format);
    }

    /* the device as delivered: --channel decides the file, not what you hear */
    aud_playback_feed(&pb, hw_buf, (size_t)got, dev);

    frames_written += (uint64_t)got;
    recorded += (uint64_t)got;
    elapsed = (double)frames_written / dev->rate;

    if (elapsed >= next_meter_at)
    {
      double peak =
          aud_format_peak(analysis, (size_t)got, shape.out_channels, dev->format);

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
             (double)(frames_written * shape.out_channels * wav_bytes) /
                 (1024.0 * 1024.0),
             xruns, meter_clipped(&meter) ? ", clipping detected" : "");
    if (meter_clipped(&meter))
    {
      aud_warn("input clipped - lower the level on the device and record again");
    }
  }

out:
  aud_playback_stop(&pb);
  if (pb.dropped > 0)
  {
    /*
     * Expected rather than wrong: capture and playback do not share a clock, so
     * the monitor skips instead of drifting further behind. Worth having under
     * -v when what was heard did not sound like what was written.
     */
    aud_debug("the monitor dropped %lu frame(s) keeping up with the input", pb.dropped);
  }

  aud_spectrum_destroy(spectrum);
  aud_preroll_free(&pre);
  free(out_buf);
  free(pick_buf);
  free(hw_buf);

  if (stats != NULL)
  {
    stats->frames = frames_written;
    stats->bytes = frames_written * shape.out_channels * wav_bytes;
    stats->preroll_frames = preroll_frames;
    stats->xruns = xruns;
    stats->monitor_dropped = pb.dropped;
    stats->clipped = meter_clipped(&meter);
    stats->interrupted = aud_signals_stop_requested();
    stats->cancelled = cancelled;
  }

  return rc;
}

/*
 * Fail before claiming the device, so a mistyped filename does not leave the
 * user staring at device warnings. wav_open() still does the authoritative,
 * race-free check when it creates the file.
 */
static int output_is_free(const char *path, int overwrite)
{
  if (overwrite || access(path, F_OK) != 0)
  {
    return 1;
  }

  aud_error("%s already exists (pass --force to overwrite)", path);
  return 0;
}

/*
 * Work out the file this invocation writes, from the name that was given and
 * the folder takes are kept in.
 *
 * The folder is created here rather than left to wav_open(): the point of
 * configuring one is not to have to think about it, and a recorder that
 * refuses because the folder it was told to use is not there yet has missed
 * that point. Only the configured folder, though - a name that says where it
 * goes still fails on a missing directory exactly as it always did, because
 * there the directory is part of what was typed.
 */
static int place_take(char *dst, size_t size, const aud_options *opts)
{
  const char *name = opts->take_prefix != NULL ? opts->take_prefix : opts->output_path;
  char prefix[AUD_PATH_MAX];

  if (aud_path_place(prefix, sizeof(prefix), opts->take_dir, name) != 0)
  {
    aud_error("'%s' in '%s' is too long a path to record to", name, opts->take_dir);
    return -1;
  }

  if (opts->take_dir[0] != '\0' && strchr(name, '/') == NULL &&
      aud_path_mkdirs(opts->take_dir) != 0)
  {
    aud_perror("cannot use %s", opts->take_dir);
    return -1;
  }

  if (opts->take_prefix == NULL)
  {
    if ((size_t)snprintf(dst, size, "%s", prefix) >= size)
    {
      aud_error("'%s' is too long a path to record to", prefix);
      return -1;
    }
    return output_is_free(dst, opts->overwrite) ? 0 : -1;
  }

  if (aud_take_next(dst, size, prefix) != 0)
  {
    aud_error("cannot pick a take name from '%s'", prefix);
    aud_info("the prefix may be too long, or the first %u takes may all exist",
             AUD_TAKE_MAX_NUMBER);
    return -1;
  }

  aud_info("recording %s", dst);
  return 0;
}

/* Whether there is any point asking where the take should be kept. */
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

  /* --quiet asked for errors only, and a question is not one */
  return opts->log_level != AUD_LOG_QUIET && aud_prompt_available();
}

/*
 * Ask where the finished take should be kept, and put it there.
 *
 * Everything here is arranged around one rule: the take has just been played,
 * it cannot be played again, and nothing this does may lose it. So the file is
 * already complete and safe on disk before the first question is asked, a
 * refused answer leaves it exactly where it is, and a move that fails says so
 * and says where the take still is.
 *
 * `path` is updated to wherever it ended up. The folder it is already in is
 * the default answer, which with a take_dir configured is that folder: the
 * common case is Enter, Enter, and the take stays where it was always going.
 */
static void store_take(char *path, size_t size)
{
  char folder[AUD_PATH_MAX];
  char name[AUD_PATH_MAX];

  if (aud_path_dirname(folder, sizeof(folder), path) != 0 ||
      snprintf(name, sizeof(name), "%s", aud_path_basename(path)) < 0)
  {
    return;
  }

  for (;;)
  {
    char answer[AUD_PATH_MAX];
    char target[AUD_PATH_MAX];
    char shown[AUD_PATH_MAX];

    /*
     * Offered with '~' back in it, and expanded again out of the answer. A
     * home directory written out in full is most of the width of the line and
     * none of what it says.
     */
    if (aud_path_shorten(shown, sizeof(shown), folder) != 0)
    {
      snprintf(shown, sizeof(shown), "%s", folder);
    }

    if (aud_prompt_line("folder", shown, answer, sizeof(answer)) != 0 ||
        aud_path_expand(folder, sizeof(folder), answer) != 0)
    {
      aud_info("kept %s", path);
      return;
    }

    if (aud_prompt_line("name", name, answer, sizeof(answer)) != 0 ||
        (size_t)snprintf(name, sizeof(name), "%s", answer) >= sizeof(name))
    {
      aud_info("kept %s", path);
      return;
    }

    if (aud_path_join(target, sizeof(target), folder, name) != 0)
    {
      aud_warn("that is too long a path; the take is still at %s", path);
      return;
    }

    if (strcmp(target, path) == 0)
    {
      aud_info("kept %s", path);
      return;
    }

    if (aud_path_mkdirs(folder) != 0)
    {
      aud_perror("cannot use %s", folder);
      continue;
    }

    if (aud_path_move(path, target) == 0)
    {
      snprintf(path, size, "%s", target);
      aud_info("stored %s", path);
      return;
    }

    /*
     * The one failure worth another go round: a name that is taken is a name
     * the user can change, and asking beats either clobbering the file that is
     * there or giving up on the one just recorded.
     */
    if (errno == EEXIST)
    {
      aud_warn("%s already exists", target);
      continue;
    }

    aud_perror("cannot move the take to %s", target);
    aud_info("it is still at %s", path);
    return;
  }
}

int aud_cmd_record(const aud_options *opts)
{
  aud_device_config cfg;
  aud_device dev;
  aud_recorder_options rec;
  aud_recorder_stats stats;
  char output[AUD_PATH_MAX];
  int rc;

  if (place_take(output, sizeof(output), opts) != 0)
  {
    return EXIT_FAILURE;
  }

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

  rec.output_path = output;
  rec.channel = opts->channel;
  rec.duration = opts->duration;
  /* a --take name is free by construction, so it never needs clobbering */
  rec.overwrite = opts->take_prefix != NULL ? 0 : opts->overwrite;
  rec.show_meter = opts->show_meter;
  rec.show_spectrum = opts->show_spectrum;
  rec.preroll = opts->preroll;
  rec.monitor = opts->monitor;
  rec.monitor_device = opts->monitor_device;
  rec.monitor_gain = (float)opts->monitor_gain;
  rec.click_bpm = opts->click_bpm;
  rec.click_beats = opts->click_beats;
  rec.click_subdiv = opts->click_subdiv;
  rec.click_gain = (float)opts->click_gain;
  rec.metadata = opts->metadata;
  rec.note = opts->note;

  rc = recorder_run(&dev, &rec, &stats);
  aud_device_close(&dev);

  if (rc != 0)
  {
    return EXIT_FAILURE;
  }

  /*
   * Nothing to ask about when the take was abandoned while armed: there is no
   * file, and offering to put it somewhere would be a question about a
   * recording that was never made.
   */
  if (!stats.cancelled && should_ask(opts))
  {
    store_take(output, sizeof(output));
  }

  return EXIT_SUCCESS;
}
