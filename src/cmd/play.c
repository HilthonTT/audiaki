/* SPDX-License-Identifier: MIT */
#include "cmd/cmd.h"

#include "audio/format.h"
#include "audio/spectrum.h"
#include "backend/monitor.h"
#include "media/wav.h"
#include "term/keys.h"
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
  int index;              /* which of the files given this is, counting from 1 */
  int total;              /* how many there are, so the line can say "2 of 5" */
} aud_play_options;

/*
 * How a pass over one file ended, which is also what the run around it does
 * next. Everything but PLAY_CONTINUE comes back from play_run(); that one
 * never does, being what the transport says when it has nothing to say.
 */
typedef enum
{
  PLAY_CONTINUE = 0, /* nothing has asked for this pass to end */
  PLAY_ENDED,        /* the file ran out, or -t stopped it */
  PLAY_NEXT,
  PLAY_PREV,
  PLAY_QUIT,
  PLAY_FAILED,
} play_result;

/*
 * Play `opts->input_path` until it ends, `opts->duration` elapses, a key asks
 * for something else or the stop flag is set. `keys` may be inactive, in which
 * case it plays start to finish; it is never NULL.
 */
static play_result play_run(const aud_play_options *opts, aud_keys *keys);

/* how often the meter is redrawn, in seconds of played audio */
#define PLAY_METER_INTERVAL 0.05

/*
 * How much is handed to the output at a time. Smaller than the buffer, so the
 * meter is redrawn several times across it rather than once when it is queued.
 */
#define PLAY_CHUNK_FRAMES 512u

/*
 * How long to wait when the output is full. The device consumes a period every
 * few milliseconds, so this is the granularity of the pacing, of how soon a
 * keypress is noticed and of the worst case delay before Ctrl+C is - all
 * comfortably below what anyone can hear or feel.
 */
#define PLAY_IDLE_SLEEP_MS 5

/* What a cursor key is worth: a phrase sideways, or a section of a take. */
#define PLAY_SEEK_NUDGE 5.0
#define PLAY_SEEK_STRIDE 30.0

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

/*
 * The meter line, in whichever of its two forms this run asked for. `since` is
 * how much played time the spectrum should decay over, and is ignored by the
 * peak bar.
 */
static void draw_meter(aud_meter *meter, aud_spectrum *spectrum, size_t bands,
                       double peak, double seconds, double since)
{
  if (spectrum != NULL)
  {
    const float *values = aud_spectrum_analyse(spectrum, since);

    meter_draw_spectrum(meter, values, bands, peak, seconds, 0);
    return;
  }
  meter_draw(meter, peak, seconds, 0);
}

/*
 * Move `seconds` from where the file is being read, clamped to it.
 *
 * Relative to what has been handed to the output, which used to be a buffer's
 * worth ahead of what was coming out of it - so a jump landed about a tenth of
 * a second from where it was asked for. The queue is now dropped along with
 * the jump, which makes the two the same place.
 */
static void seek_by(aud_monitor *mon, wav_reader *wav, double seconds, int *moved)
{
  double target = (double)wav->position / wav->rate + seconds;

  if (target < 0.0)
  {
    target = 0.0;
  }

  if (wav_read_seek(wav, (uint64_t)(target * wav->rate + 0.5)) == 0)
  {
    aud_monitor_flush(mon);
    *moved = 1;
  }
}

/*
 * Take everything typed since the last time round the loop and do it.
 *
 * Returns PLAY_CONTINUE when the pass carries on, which is nearly always, and
 * sets `*moved` when the position or the pause state changed - the caller
 * redraws on that, because a line still describing the moment before the key
 * was pressed is worse than no line at all.
 */
static play_result apply_keys(aud_keys *keys, aud_monitor *mon, wav_reader *wav,
                              int *paused, int *moved)
{
  aud_key key;

  while ((key = aud_keys_poll(keys)) != AUD_KEY_NONE)
  {
    switch (key)
    {
    case AUD_KEY_SPACE:
      *paused = !*paused;
      /*
       * Pausing drops the queue for the same reason a seek does: otherwise
       * "stop" is followed by the buffer's worth still in flight. Resuming
       * carries on from where the file is, which is where the display has
       * been saying it was all along.
       */
      if (*paused)
      {
        aud_monitor_flush(mon);
      }
      *moved = 1;
      break;
    case AUD_KEY_LEFT:
      seek_by(mon, wav, -PLAY_SEEK_NUDGE, moved);
      break;
    case AUD_KEY_RIGHT:
      seek_by(mon, wav, PLAY_SEEK_NUDGE, moved);
      break;
    case AUD_KEY_DOWN:
      seek_by(mon, wav, -PLAY_SEEK_STRIDE, moved);
      break;
    case AUD_KEY_UP:
      seek_by(mon, wav, PLAY_SEEK_STRIDE, moved);
      break;
    case AUD_KEY_HOME:
      if (wav_read_seek(wav, 0) == 0)
      {
        aud_monitor_flush(mon);
        *moved = 1;
      }
      break;
    /*
     * Seeked to rather than returned from, so the file finishes the way it
     * would have on its own - the run reports what was played and moves on
     * rather than abandoning the pass. 'n' is the key for leaving one early.
     *
     * The queue still goes, like any other jump: what is in it is audio from
     * before the end, and hearing a tenth of a second of it after pressing End
     * is the thing this whole set of flushes is here to stop.
     */
    case AUD_KEY_END:
      if (wav_read_seek(wav, wav->frames) == 0)
      {
        aud_monitor_flush(mon);
        *moved = 1;
      }
      break;
    case AUD_KEY_NEXT:
      return PLAY_NEXT;
    case AUD_KEY_PREV:
      return PLAY_PREV;
    case AUD_KEY_QUIT:
      return PLAY_QUIT;
    case AUD_KEY_NONE:
    case AUD_KEY_OTHER:
    default:
      break;
    }
  }

  return PLAY_CONTINUE;
}

static void describe(const wav_reader *wav, const aud_play_options *opts)
{
  char where[32];

  /* the position in the list, and nothing at all when there is no list */
  if (opts->total > 1)
  {
    snprintf(where, sizeof(where), "[%d/%d] ", opts->index, opts->total);
  }
  else
  {
    where[0] = '\0';
  }

  aud_info("playing %s%s: %.2f s, %u Hz, %u ch, %u-bit %s", where, opts->input_path,
           wav_read_duration(wav), wav->rate, wav->channels, wav->bits,
           wav->is_float ? "float" : "PCM");
}

/*
 * One pass over one file: what was opened for it, and where it has got to.
 * Gathered so the setup, the loop and the single teardown can be read apart.
 */
typedef struct
{
  const aud_play_options *opts;
  wav_reader wav;
  aud_meter meter;
  aud_monitor *mon;
  aud_spectrum *spectrum;
  float *frames;
  float *mono;
  size_t bands;
  uint64_t written; /* frames handed to the output, which is what was heard */
  uint64_t limit;
  int paused;
} play_state;

/* The file, and how much of it -t leaves. Nothing to clean up when this fails. */
static int play_open(play_state *p)
{
  const aud_play_options *opts = p->opts;

  if (wav_read_open(&p->wav, opts->input_path) != 0)
  {
    aud_error("cannot play %s: %s", opts->input_path,
              p->wav.error != NULL ? p->wav.error : "unreadable");
    return -1;
  }

  if (p->wav.frames == 0 || p->wav.channels == 0 || p->wav.rate == 0)
  {
    aud_error("%s holds no audio to play", opts->input_path);
    wav_read_close(&p->wav);
    return -1;
  }

  if (opts->duration > 0.0)
  {
    p->limit = (uint64_t)(opts->duration * (double)p->wav.rate + 0.5);
    if (p->limit == 0)
    {
      p->limit = 1;
    }
  }

  /* initialised before any early exit so the cleanup path is unconditional */
  meter_init(&p->meter, opts->show_meter);
  meter_set_total(&p->meter, wav_read_duration(&p->wav));
  return 0;
}

/* The band analyser, when the terminal has room for one and asked for it. */
static void play_open_spectrum(play_state *p)
{
  aud_spectrum_config spec_cfg;

  if (!p->opts->show_spectrum || !p->opts->show_meter)
  {
    return;
  }

  p->bands = meter_fit_bands(&p->meter);
  if (p->bands == 0)
  {
    return;
  }

  aud_spectrum_config_defaults(&spec_cfg, p->wav.rate, p->bands);
  p->spectrum = aud_spectrum_create(&spec_cfg);
  p->mono = malloc((size_t)PLAY_CHUNK_FRAMES * sizeof(*p->mono));

  if (p->spectrum == NULL || p->mono == NULL)
  {
    /* not worth refusing to play over: fall back to the peak bar */
    aud_warn("cannot set up the spectrum display, showing the peak meter");
    aud_spectrum_destroy(p->spectrum);
    p->spectrum = NULL;
    p->bands = 0;
  }
}

static int play_setup(play_state *p)
{
  aud_monitor_config mon_cfg;

  p->frames = malloc((size_t)PLAY_CHUNK_FRAMES * p->wav.channels * sizeof(*p->frames));
  if (p->frames == NULL)
  {
    aud_error("cannot allocate a playback buffer for %u channels", p->wav.channels);
    return -1;
  }

  play_open_spectrum(p);

  aud_monitor_config_defaults(&mon_cfg, p->wav.rate, p->wav.channels);
  if (p->opts->device != NULL)
  {
    mon_cfg.name = p->opts->device;
  }

  p->mon = aud_monitor_open(&mon_cfg);
  if (p->mon == NULL)
  {
    /* the backend has already said which part of opening the output failed */
    aud_error("cannot play %s", p->opts->input_path);
    return -1;
  }
  return 0;
}

static void play_free(play_state *p)
{
  meter_clear(&p->meter);
  aud_monitor_close(p->mon);
  aud_spectrum_destroy(p->spectrum);
  free(p->mono);
  free(p->frames);
  wav_read_close(&p->wav);
}

/*
 * How many frames to read next, given the output's room and what -t leaves.
 * Zero means the pass is over.
 */
static size_t play_want(const play_state *p, long space)
{
  size_t want = (size_t)space < PLAY_CHUNK_FRAMES ? (size_t)space : PLAY_CHUNK_FRAMES;

  /* read only what is still missing so -t lands on an exact frame count */
  if (p->limit != 0)
  {
    if (p->wav.position >= p->limit)
    {
      return 0;
    }
    if (p->wav.position + want > p->limit)
    {
      want = (size_t)(p->limit - p->wav.position);
    }
  }
  return want;
}

static play_result play_loop(play_state *p, aud_keys *keys)
{
  double next_meter_at = 0.0;
  double last_drawn_at = 0.0;

  while (!aud_signals_stop_requested())
  {
    long space;
    long got;
    size_t want;
    double elapsed;
    int moved = 0;
    play_result asked = apply_keys(keys, p->mon, &p->wav, &p->paused, &moved);

    if (asked != PLAY_CONTINUE)
    {
      /*
       * Left without draining: skipping to the next file is asking for it now,
       * and a buffer's worth of the one being left over the top of it is not
       * what was wanted.
       */
      meter_clear(&p->meter);
      return asked;
    }

    elapsed = (double)p->wav.position / p->wav.rate;

    if (moved)
    {
      meter_set_paused(&p->meter, p->paused);
      draw_meter(&p->meter, p->spectrum, p->bands, 0.0, elapsed, 0.0);
      last_drawn_at = elapsed;
      next_meter_at = elapsed + PLAY_METER_INTERVAL;
    }

    if (p->paused)
    {
      sleep_ms(PLAY_IDLE_SLEEP_MS);
      continue;
    }

    space = aud_monitor_space(p->mon);
    if (space < 0)
    {
      meter_clear(&p->meter);
      aud_error("playback stopped: the output failed");
      return PLAY_FAILED;
    }
    if (space == 0)
    {
      sleep_ms(PLAY_IDLE_SLEEP_MS);
      continue;
    }

    want = play_want(p, space);
    if (want == 0)
    {
      break;
    }

    got = wav_read_frames(&p->wav, p->frames, want);
    if (got < 0)
    {
      meter_clear(&p->meter);
      aud_error("cannot read %s: %s", p->opts->input_path,
                p->wav.error != NULL ? p->wav.error : "read failed");
      return PLAY_FAILED;
    }
    if (got == 0)
    {
      break;
    }

    if (aud_monitor_write(p->mon, p->frames, (size_t)got, 1.0f) != 0)
    {
      meter_clear(&p->meter);
      aud_error("playback stopped: the output failed");
      return PLAY_FAILED;
    }

    if (p->spectrum != NULL)
    {
      to_mono(p->mono, p->frames, (size_t)got, p->wav.channels);
      aud_spectrum_push(p->spectrum, p->mono, (size_t)got);
    }

    p->written += (uint64_t)got;
    elapsed = (double)p->wav.position / p->wav.rate;

    if (elapsed >= next_meter_at)
    {
      /*
       * Smooth against played time rather than PLAY_METER_INTERVAL, so the
       * bars decay at the same rate whether or not a redraw was skipped.
       */
      draw_meter(&p->meter, p->spectrum, p->bands,
                 peak_of(p->frames, (size_t)got * p->wav.channels), elapsed,
                 elapsed - last_drawn_at);

      last_drawn_at = elapsed;
      next_meter_at = elapsed + PLAY_METER_INTERVAL;
    }
  }

  return PLAY_ENDED;
}

/* The tail of a pass that ran to its own end, rather than being left. */
static void play_finish(play_state *p)
{
  unsigned long dropped;

  /*
   * The last buffer's worth has been handed over but not heard yet. Closing the
   * output here would cut the end of the file off; an interruption is asking
   * for exactly that, so it skips the wait.
   */
  if (!aud_signals_stop_requested())
  {
    aud_monitor_drain(p->mon);
  }

  meter_clear(&p->meter);

  dropped = aud_monitor_dropped(p->mon);
  if (dropped > 0)
  {
    /* pacing on aud_monitor_space() should prevent this; say so if it did not */
    aud_warn("%lu frame(s) dropped: the output could not keep up", dropped);
  }

  aud_info("played %.2f s of %s%s", (double)p->written / p->wav.rate, p->opts->input_path,
           aud_signals_stop_requested() ? " (stopped early)" : "");
}

static play_result play_run(const aud_play_options *opts, aud_keys *keys)
{
  play_state p;
  play_result rc = PLAY_FAILED;

  memset(&p, 0, sizeof(p));
  p.opts = opts;

  if (play_open(&p) != 0)
  {
    return PLAY_FAILED;
  }

  if (play_setup(&p) != 0)
  {
    goto out;
  }

  describe(&p.wav, opts);
  if (p.limit != 0)
  {
    aud_info("stopping after %.2f s", opts->duration);
  }

  rc = play_loop(&p, keys);

  /* only the natural end drains and reports; a key asking for another file is
   * asking for it now, and apply_keys() never returns PLAY_ENDED */
  if (rc == PLAY_ENDED)
  {
    play_finish(&p);
  }

out:
  play_free(&p);
  return rc;
}

/* The `i`th file of the run, counting the one --play itself named as zero. */
static const char *path_at(const aud_options *opts, int i)
{
  if (i <= 0)
  {
    return opts->input_path;
  }
  return opts->extra_inputs[i - 1];
}

/*
 * Put `order` into a random permutation of itself, Fisher-Yates.
 *
 * rand() is the right tool here and would not be for anything else: the worst a
 * predictable shuffle can do is play a familiar sequence of somebody's own
 * files. Called again for each pass over a repeating list, so a long run does
 * not settle into one order.
 */
static void shuffle_order(int *order, int total)
{
  for (int i = total - 1; i > 0; i--)
  {
    int j = rand() % (i + 1);
    int swap = order[i];

    order[i] = order[j];
    order[j] = swap;
  }
}

int aud_cmd_play(const aud_options *opts)
{
  aud_play_options play;
  aud_keys keys;
  int total = 1 + opts->extra_input_count;
  int at = 0;
  int failures = 0;
  int failures_this_pass = 0;
  int quit = 0;
  int *order;

  /*
   * The default output, unless an output was actually named. -D otherwise
   * defaults to a capture device, and $AUDIAKI_DEVICE is one by definition;
   * either would be handed to the playback side as though it could play.
   */
  play.device = opts->device_explicit ? opts->device : NULL;
  play.duration = opts->duration;
  play.show_meter = opts->show_meter;
  play.show_spectrum = opts->show_spectrum;
  play.total = total;

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  /*
   * The order the files are walked in, which is the order they were given
   * until --shuffle says otherwise. Indirecting through it is what lets 'p'
   * step back through a shuffled list rather than back through argv.
   */
  order = malloc((size_t)total * sizeof(*order));
  if (order == NULL)
  {
    aud_error("cannot allocate a playlist of %d file(s)", total);
    return EXIT_FAILURE;
  }
  for (int i = 0; i < total; i++)
  {
    order[i] = i;
  }
  if (opts->shuffle)
  {
    srand((unsigned)time(NULL));
    shuffle_order(order, total);
  }

  /*
   * Said once, up front. Both change when the run ends, and a run that will not
   * end on its own is worth knowing about before it does not.
   */
  if (opts->repeat == AUD_REPEAT_ONE)
  {
    aud_info("repeating each file until stopped; n moves on");
  }
  else if (opts->repeat == AUD_REPEAT_ALL)
  {
    aud_info("repeating %s until stopped", total > 1 ? "the playlist" : "the file");
  }
  if (opts->shuffle && total > 1)
  {
    aud_info("playing %d files in a shuffled order", total);
  }

  /*
   * Opened once around the whole run rather than per file: switching the
   * terminal over between two files would drop whatever was typed in the gap,
   * which is exactly when somebody is reaching for 'n' again.
   *
   * A failure here is not one: it means there is nobody at a keyboard, and
   * playback then does what it has always done and runs to the end.
   */
  if (aud_keys_open(&keys) == 0)
  {
    aud_info("space pauses, left/right seek 5 s, up/down 30 s, "
             "n/p change file, q stops");
  }
  else
  {
    aud_info("press Ctrl+C to stop");
  }

  while (!quit && !aud_signals_stop_requested())
  {
    play.input_path = path_at(opts, order[at]);
    play.index = at + 1;

    switch (play_run(&play, &keys))
    {
    case PLAY_PREV:
      /*
       * The first file has nothing before it, so 'p' there starts it again -
       * unless the list repeats, in which case what is before the first file is
       * the last one.
       */
      if (at > 0)
      {
        at--;
      }
      else if (opts->repeat == AUD_REPEAT_ALL)
      {
        at = total - 1;
      }
      break;
    case PLAY_QUIT:
      quit = 1;
      break;
    case PLAY_FAILED:
      /*
       * One unreadable file does not end a playlist - the rest of it is still
       * worth hearing - but the run still reports that something was wrong.
       * Counted per pass as well as in total, because --repeat over a list that
       * is entirely unreadable would otherwise spin forever failing.
       */
      failures++;
      failures_this_pass++;
      at++;
      break;
    case PLAY_NEXT:
      /* what 'n' is for, and the one way off a file that repeats on its own */
      at++;
      break;
    case PLAY_ENDED:
    case PLAY_CONTINUE:
    default:
      if (opts->repeat != AUD_REPEAT_ONE)
      {
        at++;
      }
      break;
    }

    if (at >= total)
    {
      if (opts->repeat == AUD_REPEAT_NONE || failures_this_pass >= total)
      {
        if (failures_this_pass >= total && opts->repeat != AUD_REPEAT_NONE)
        {
          aud_error("nothing in the playlist could be played; not repeating it");
        }
        quit = 1;
      }
      else
      {
        at = 0;
        failures_this_pass = 0;
        if (opts->shuffle)
        {
          shuffle_order(order, total);
        }
      }
    }
  }

  aud_keys_close(&keys);
  free(order);

  return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
