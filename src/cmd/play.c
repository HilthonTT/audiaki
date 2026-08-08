/* SPDX-License-Identifier: MIT */
#include "cmd/cmd.h"

#include "audio/format.h"
#include "audio/spectrum.h"
#include "backend/monitor.h"
#include "media/wav.h"
#include "term/meter.h"
#include "util/log.h"
#include "util/signals.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * What a playback run is asked for. Filled in from aud_options by
 * aud_cmd_play() at the bottom of this file.
 *
 * No capture device is opened. The output is aud_monitor, which is the playback
 * PCM the desktop app monitors through; see monitor.h for why a file has to ask
 * how much will fit rather than write as fast as it can read.
 */
typedef struct
{
  const char *input_path; /* the WAV to play */
  const char *device;     /* playback device; NULL means the default output */
  double duration;        /* seconds; 0 = to the end of the file */
  int show_meter;         /* draw the level line at all */
  int show_spectrum;      /* spectrum bars instead of the peak bar */
} aud_play_options;

/*
 * Play `opts->input_path` until it ends, `opts->duration` elapses or the stop
 * flag is set. Returns 0 on success and -1 on failure, after reporting the
 * reason through log.h.
 */
static int play_run(const aud_play_options *opts);

/* how often the meter is redrawn, in seconds of played audio */
#define PLAY_METER_INTERVAL 0.05

/*
 * How much is handed to the output at a time. Smaller than the buffer, so the
 * meter is redrawn several times across it rather than once when it is queued.
 */
#define PLAY_CHUNK_FRAMES 512u

/*
 * How long to wait when the output is full. The device consumes a period every
 * few milliseconds, so this is the granularity of the pacing and the worst case
 * delay before Ctrl+C is noticed - both comfortably below what anyone can hear
 * or feel.
 */
#define PLAY_IDLE_SLEEP_MS 5

static void sleep_ms(long ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/*
 * Absolute peak of interleaved floats. Not aud_format_peak(), which reads the
 * capture formats: what came out of the WAV reader has already been decoded,
 * and float WAV is allowed to exceed full scale, so this does not clamp.
 */
static double peak_of(const float *samples, size_t count)
{
  double peak = 0.0;

  for (size_t i = 0; i < count; i++)
  {
    double v = fabs((double)samples[i]);

    if (v > peak)
    {
      peak = v;
    }
  }
  return peak;
}

static void to_mono(float *dst, const float *src, size_t frames, unsigned channels)
{
  for (size_t f = 0; f < frames; f++)
  {
    float sum = 0.0f;

    for (unsigned ch = 0; ch < channels; ch++)
    {
      sum += src[f * channels + ch];
    }
    dst[f] = sum / (float)channels;
  }
}

static void describe(const wav_reader *wav, const char *path)
{
  aud_info("playing %s: %.2f s, %u Hz, %u ch, %u-bit %s", path, wav_read_duration(wav),
           wav->rate, wav->channels, wav->bits, wav->is_float ? "float" : "PCM");
}

static int play_run(const aud_play_options *opts)
{
  wav_reader wav;
  aud_meter meter;
  aud_monitor_config mon_cfg;
  aud_monitor *mon = NULL;
  aud_spectrum *spectrum = NULL;
  float *frames = NULL;
  float *mono = NULL;
  size_t bands = 0;
  uint64_t played = 0;
  uint64_t limit = 0;
  unsigned long dropped;
  double next_meter_at = 0.0;
  double last_drawn_at = 0.0;
  int rc = -1;

  if (wav_read_open(&wav, opts->input_path) != 0)
  {
    aud_error("cannot play %s: %s", opts->input_path,
              wav.error != NULL ? wav.error : "unreadable");
    return -1;
  }

  if (wav.frames == 0 || wav.channels == 0 || wav.rate == 0)
  {
    aud_error("%s holds no audio to play", opts->input_path);
    wav_read_close(&wav);
    return -1;
  }

  if (opts->duration > 0.0)
  {
    limit = (uint64_t)(opts->duration * (double)wav.rate + 0.5);
    if (limit == 0)
    {
      limit = 1;
    }
  }

  /* initialised before any early exit so the cleanup path is unconditional */
  meter_init(&meter, opts->show_meter);
  meter_set_total(&meter, wav_read_duration(&wav));

  frames = malloc((size_t)PLAY_CHUNK_FRAMES * wav.channels * sizeof(*frames));
  if (frames == NULL)
  {
    aud_error("cannot allocate a playback buffer for %u channels", wav.channels);
    goto out;
  }

  if (opts->show_spectrum && opts->show_meter)
  {
    aud_spectrum_config spec_cfg;

    bands = meter_fit_bands(&meter);
    if (bands > 0)
    {
      aud_spectrum_config_defaults(&spec_cfg, wav.rate, bands);
      spectrum = aud_spectrum_create(&spec_cfg);
      mono = malloc((size_t)PLAY_CHUNK_FRAMES * sizeof(*mono));

      if (spectrum == NULL || mono == NULL)
      {
        /* not worth refusing to play over: fall back to the peak bar */
        aud_warn("cannot set up the spectrum display, showing the peak meter");
        aud_spectrum_destroy(spectrum);
        spectrum = NULL;
        bands = 0;
      }
    }
  }

  aud_monitor_config_defaults(&mon_cfg, wav.rate, wav.channels);
  if (opts->device != NULL)
  {
    mon_cfg.name = opts->device;
  }

  mon = aud_monitor_open(&mon_cfg);
  if (mon == NULL)
  {
    /* the backend has already said which part of opening the output failed */
    aud_error("cannot play %s", opts->input_path);
    goto out;
  }

  describe(&wav, opts->input_path);
  if (limit != 0)
  {
    aud_info("stopping after %.2f s (Ctrl+C stops early)", opts->duration);
  }
  else
  {
    aud_info("press Ctrl+C to stop");
  }

  while (!aud_signals_stop_requested())
  {
    long space = aud_monitor_space(mon);
    long got;
    size_t want;
    double elapsed;

    if (space < 0)
    {
      meter_clear(&meter);
      aud_error("playback stopped: the output failed");
      goto out;
    }
    if (space == 0)
    {
      sleep_ms(PLAY_IDLE_SLEEP_MS);
      continue;
    }

    want = (size_t)space < PLAY_CHUNK_FRAMES ? (size_t)space : PLAY_CHUNK_FRAMES;

    /* read only what is still missing so -t lands on an exact frame count */
    if (limit != 0 && played + want > limit)
    {
      want = (size_t)(limit - played);
    }
    if (want == 0)
    {
      break;
    }

    got = wav_read_frames(&wav, frames, want);
    if (got < 0)
    {
      meter_clear(&meter);
      aud_error("cannot read %s: %s", opts->input_path,
                wav.error != NULL ? wav.error : "read failed");
      goto out;
    }
    if (got == 0)
    {
      break;
    }

    if (aud_monitor_write(mon, frames, (size_t)got, 1.0f) != 0)
    {
      meter_clear(&meter);
      aud_error("playback stopped: the output failed");
      goto out;
    }

    if (spectrum != NULL)
    {
      to_mono(mono, frames, (size_t)got, wav.channels);
      aud_spectrum_push(spectrum, mono, (size_t)got);
    }

    played += (uint64_t)got;
    elapsed = (double)played / wav.rate;

    if (elapsed >= next_meter_at)
    {
      double peak = peak_of(frames, (size_t)got * wav.channels);

      if (spectrum != NULL)
      {
        /*
         * Smooth against played time rather than PLAY_METER_INTERVAL, so the
         * bars decay at the same rate whether or not a redraw was skipped.
         */
        const float *values = aud_spectrum_analyse(spectrum, elapsed - last_drawn_at);
        meter_draw_spectrum(&meter, values, bands, peak, elapsed, 0);
      }
      else
      {
        meter_draw(&meter, peak, elapsed, 0);
      }

      last_drawn_at = elapsed;
      next_meter_at = elapsed + PLAY_METER_INTERVAL;
    }

    if (limit != 0 && played >= limit)
    {
      break;
    }
  }

  /*
   * The last buffer's worth has been handed over but not heard yet. Closing the
   * output here would cut the end of the file off; an interruption is asking
   * for exactly that, so it skips the wait.
   */
  if (!aud_signals_stop_requested())
  {
    aud_monitor_drain(mon);
  }

  meter_clear(&meter);
  rc = 0;

  dropped = aud_monitor_dropped(mon);
  if (dropped > 0)
  {
    /* pacing on aud_monitor_space() should prevent this; say so if it did not */
    aud_warn("%lu frame(s) dropped: the output could not keep up", dropped);
  }

  aud_info("played %.2f s of %s%s", (double)played / wav.rate, opts->input_path,
           aud_signals_stop_requested() ? " (stopped early)" : "");

out:
  meter_clear(&meter);
  aud_monitor_close(mon);
  aud_spectrum_destroy(spectrum);
  free(mono);
  free(frames);
  wav_read_close(&wav);

  return rc;
}

int aud_cmd_play(const aud_options *opts)
{
  aud_play_options play;

  play.input_path = opts->input_path;
  /*
   * The default output, unless an output was actually named. -D otherwise
   * defaults to a capture device, and $AUDIAKI_DEVICE is one by definition;
   * either would be handed to the playback side as though it could play.
   */
  play.device = opts->device_explicit ? opts->device : NULL;
  play.duration = opts->duration;
  play.show_meter = opts->show_meter;
  play.show_spectrum = opts->show_spectrum;

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  return play_run(&play) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
