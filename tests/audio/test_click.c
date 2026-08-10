/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "audio/click.h"

#include <stdlib.h>
#include <string.h>

#define RATE 48000u
#define FRAMES 200000u

static float g_buf[FRAMES];
static float g_alt[FRAMES];

/* Render `frames` of mono click into `buf`, handing it over `chunk` at a time. */
static void render(aud_click *c, float *buf, size_t frames, size_t chunk)
{
  size_t at;

  memset(buf, 0, frames * sizeof(*buf));

  for (at = 0; at < frames; at += chunk)
  {
    size_t n = (frames - at < chunk) ? frames - at : chunk;
    aud_click_mix(c, buf + at, n, 1u);
  }
}

static void start(aud_click *c, double bpm, unsigned beats_per_bar, float gain)
{
  aud_click_config cfg;

  aud_click_config_defaults(&cfg, bpm, RATE);
  cfg.beats_per_bar = beats_per_bar;
  cfg.gain = gain;
  CHECK_EQ_INT(aud_click_init(c, &cfg), 0);
}

static void start_divided(aud_click *c, double bpm, unsigned beats_per_bar,
                          unsigned subdiv, float gain)
{
  aud_click_config cfg;

  aud_click_config_defaults(&cfg, bpm, RATE);
  cfg.beats_per_bar = beats_per_bar;
  cfg.subdiv = subdiv;
  cfg.gain = gain;
  CHECK_EQ_INT(aud_click_init(c, &cfg), 0);
}

/* The largest absolute sample in [at, at + n), which is a strike's peak. */
static double burst_peak(const float *buf, size_t at, size_t n)
{
  double worst = 0.0;

  for (size_t i = at; i < at + n; i++)
  {
    double v = buf[i] < 0.0f ? -(double)buf[i] : (double)buf[i];

    if (v > worst)
    {
      worst = v;
    }
  }
  return worst;
}

TEST(beats_land_on_the_frame_the_tempo_says)
{
  aud_click c;

  start(&c, 120.0, 4u, 1.0f);

  /* two beats a second at 48 kHz */
  CHECK_EQ_INT((long long)aud_click_beat_frames(&c), 24000);

  render(&c, g_buf, 96000u, 1024u);

  for (unsigned n = 0; n < 4u; n++)
  {
    size_t onset = (size_t)n * 24000u;

    /*
     * The burst opens with sin(0), so the onset frame itself is zero and the
     * one after it is not. Testing both is what pins the beat to a frame
     * rather than to a neighbourhood.
     */
    CHECK_EQ_DBL(g_buf[onset], 0.0, 0.0);
    CHECK(g_buf[onset + 1u] != 0.0f);

    if (n > 0)
    {
      CHECK_EQ_DBL(g_buf[onset - 1u], 0.0, 0.0);
    }

    /* and nothing at all between beats */
    CHECK_EQ_DBL(g_buf[onset + 12000u], 0.0, 0.0);
  }
}

TEST(the_grid_does_not_drift_across_buffers)
{
  aud_click one;
  aud_click many;
  size_t differing = 0;

  start(&one, 137.0, 4u, 1.0f); /* a tempo whose beat is not a whole frame count */
  start(&many, 137.0, 4u, 1.0f);

  render(&one, g_buf, FRAMES, FRAMES);
  render(&many, g_alt, FRAMES, 137u); /* odd chunks, landing mid-burst */

  /*
   * Bit for bit, not merely close: every frame is computed from its absolute
   * index, so how the stream was cut up cannot enter into the arithmetic. A
   * generator that accumulated per period would fail this by a sample or two
   * after four seconds, and by more the longer it ran.
   */
  for (size_t i = 0; i < FRAMES; i++)
  {
    if (g_buf[i] != g_alt[i])
    {
      differing++;
    }
  }
  CHECK_EQ_INT((long long)differing, 0);
}

TEST(the_first_beat_of_the_bar_is_accented)
{
  aud_click c;
  float accent;
  float plain;

  start(&c, 120.0, 4u, 1.0f);
  render(&c, g_buf, 120000u, 1024u);

  /* an octave up reaches twice as far into the first sample of the burst */
  accent = g_buf[1];
  plain = g_buf[24000u + 1u];
  CHECK(accent > plain * 1.5f);

  /* and the accent comes back a bar later, not before */
  CHECK_EQ_DBL(g_buf[4u * 24000u + 1u], accent, 1e-6);
  CHECK_EQ_DBL(g_buf[2u * 24000u + 1u], plain, 1e-6);
}

TEST(a_bare_pulse_has_no_accent)
{
  aud_click c;

  start(&c, 120.0, 1u, 1.0f);
  render(&c, g_buf, 72000u, 1024u);

  CHECK_EQ_DBL(g_buf[1], g_buf[24000u + 1u], 1e-9);
  CHECK_EQ_DBL(g_buf[1], g_buf[48000u + 1u], 1e-9);

  /* zero beats to a bar means the same thing as one */
  start(&c, 120.0, 0u, 1.0f);
  render(&c, g_alt, 72000u, 1024u);
  CHECK_EQ_DBL(g_alt[1], g_alt[24000u + 1u], 1e-9);
}

TEST(a_beat_decays_and_stays_inside_full_scale)
{
  aud_click c;
  float peak = 0.0f;
  size_t last_sounding = 0;

  start(&c, 120.0, 1u, 1.0f);
  render(&c, g_buf, 24000u, 1024u);

  for (size_t i = 0; i < 24000u; i++)
  {
    float mag = g_buf[i] < 0.0f ? -g_buf[i] : g_buf[i];

    if (mag > peak)
    {
      peak = mag;
    }
    if (mag != 0.0f)
    {
      last_sounding = i;
    }
  }

  /* a gain of 1.0 means a beat that reaches full scale and does not pass it */
  CHECK(peak <= 1.0f);
  CHECK(peak > 0.9f);

  /* 50 ms of burst, and silence from there to the next beat */
  CHECK(last_sounding < (size_t)(0.050 * RATE) + 1u);
}

TEST(gain_scales_the_beat_and_zero_silences_it)
{
  aud_click c;

  start(&c, 120.0, 1u, 1.0f);
  render(&c, g_buf, 4000u, 512u);

  start(&c, 120.0, 1u, 0.25f);
  render(&c, g_alt, 4000u, 512u);

  CHECK_EQ_DBL(g_alt[1], g_buf[1] * 0.25, 1e-6);

  start(&c, 120.0, 1u, 0.0f);
  render(&c, g_alt, 4000u, 512u);
  for (size_t i = 0; i < 4000u; i++)
  {
    if (g_alt[i] != 0.0f)
    {
      CHECK(0);
      break;
    }
  }
}

TEST(every_channel_gets_the_beat_and_nothing_is_overwritten)
{
  aud_click c;
  float stereo[8];

  start(&c, 120.0, 1u, 1.0f);

  for (unsigned i = 0; i < 8u; i++)
  {
    stereo[i] = 0.25f; /* what a monitored input would already have put there */
  }

  aud_click_mix(&c, stereo, 4u, 2u);

  for (unsigned f = 0; f < 4u; f++)
  {
    /* the same beat in both channels... */
    CHECK_EQ_DBL(stereo[f * 2u], stereo[f * 2u + 1u], 0.0);
    /* ...added to what was there, not written over it */
    CHECK(stereo[f * 2u] >= 0.25f);
  }
  CHECK_EQ_DBL(stereo[0], 0.25, 1e-9); /* frame 0 is sin(0) plus the input */
  CHECK(stereo[2] > 0.25f);
}

TEST(reset_puts_the_grid_back_to_the_first_beat)
{
  aud_click c;

  start(&c, 90.0, 4u, 1.0f);
  render(&c, g_buf, 60000u, 1024u);

  aud_click_reset(&c);
  render(&c, g_alt, 60000u, 1024u);

  for (size_t i = 0; i < 60000u; i++)
  {
    if (g_buf[i] != g_alt[i])
    {
      CHECK(0);
      break;
    }
  }
}

TEST(unusable_tempos_are_refused)
{
  aud_click c;
  aud_click_config cfg;

  aud_click_config_defaults(&cfg, 0.0, RATE);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), -1);

  aud_click_config_defaults(&cfg, AUD_CLICK_BPM_MIN - 0.5, RATE);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), -1);

  aud_click_config_defaults(&cfg, AUD_CLICK_BPM_MAX + 0.5, RATE);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), -1);

  aud_click_config_defaults(&cfg, AUD_CLICK_BPM_MIN, RATE);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), 0);

  aud_click_config_defaults(&cfg, AUD_CLICK_BPM_MAX, RATE);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), 0);

  /* a rate of zero would be a division, not a slow metronome */
  aud_click_config_defaults(&cfg, 120.0, 0u);
  CHECK_EQ_INT(aud_click_init(&c, &cfg), -1);
}

TEST(seeking_lands_on_the_same_grid_as_playing_up_to_it)
{
  aud_click straight;
  aud_click sought;

  start(&straight, 137.0, 4u, 1.0f);
  render(&straight, g_buf, 96000u, 1024u);

  /*
   * The whole point of a seek: a transport that jumped into the middle of a
   * project has to hear the beat the grid says is there, not a new beat one.
   * A tempo that does not divide the rate, so a walked grid and a computed
   * one would have parted company by here if either rounded.
   */
  start(&sought, 137.0, 4u, 1.0f);
  aud_click_seek(&sought, 48000u);
  render(&sought, g_alt, 48000u, 1024u);

  for (size_t i = 0; i < 48000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[48000u + i], 1e-6);
  }
}

TEST(seeking_backwards_and_forwards_costs_nothing_and_stays_on_the_beat)
{
  aud_click c;
  size_t at;

  start(&c, 120.0, 4u, 1.0f);

  /*
   * Chunk by chunk, seeking to each one before mixing it - which is what a
   * looping transport does every pass. The result has to be the plain grid.
   */
  render(&c, g_buf, 96000u, 4096u);

  memset(g_alt, 0, sizeof(g_alt));
  for (at = 0; at < 96000u; at += 4096u)
  {
    aud_click_seek(&c, at);
    aud_click_mix(&c, g_alt + at, 4096u, 1u);
  }

  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-6);
  }

  /* and a seek past the end of everything is still on the grid */
  aud_click_seek(&c, 24000u * 1000u);
  CHECK_EQ_INT((long long)c.tick, 1000);
}

TEST(a_subdivision_puts_a_tick_between_the_beats)
{
  aud_click c;

  /* 120 BPM at 48 kHz is 24000 frames a beat, so eighths are 12000 apart */
  start_divided(&c, 120.0, 4u, 2u, 1.0f);
  CHECK_EQ_INT((long long)aud_click_beat_frames(&c), 24000);

  render(&c, g_buf, 96000u, 1024u);

  for (unsigned n = 0; n < 8u; n++)
  {
    size_t onset = (size_t)n * 12000u;

    /* sin(0) opens every strike, beat or not, so the onset frame is silent */
    CHECK_EQ_DBL(g_buf[onset], 0.0, 0.0);
    CHECK(g_buf[onset + 1u] != 0.0f);
  }

  /* and nothing is struck a quarter of the way between two ticks */
  CHECK_EQ_DBL(g_buf[6000], 0.0, 0.0);
  CHECK_EQ_DBL(g_buf[18000], 0.0, 0.0);
}

TEST(subdivision_ticks_are_quieter_than_the_beats_around_them)
{
  aud_click c;
  double downbeat;
  double beat;
  double tick;

  start_divided(&c, 120.0, 4u, 2u, 1.0f);
  render(&c, g_buf, 96000u, 1024u);

  /* the burst is 50 ms, which is 2400 frames, and well clear of the next tick */
  downbeat = burst_peak(g_buf, 0u, 2400u); /* bar one, beat one */
  tick = burst_peak(g_buf, 12000u, 2400u); /* the off-beat after it */
  beat = burst_peak(g_buf, 24000u, 2400u); /* beat two */

  CHECK(downbeat > 0.0);
  CHECK(beat > 0.0);
  CHECK(tick > 0.0);

  /*
   * The order that makes the pulse readable: the bar starts loudest, the plain
   * beats sit under it, and what falls between them is quieter still.
   */
  CHECK(tick < beat);
  CHECK(beat <= downbeat);
}

TEST(a_subdivision_of_one_is_the_undivided_beat)
{
  aud_click plain;
  aud_click divided;

  start(&plain, 100.0, 3u, 0.8f);
  render(&plain, g_buf, 96000u, 1024u);

  /* 0 and 1 both mean "do not divide", so both have to match the plain grid */
  start_divided(&divided, 100.0, 3u, 1u, 0.8f);
  render(&divided, g_alt, 96000u, 1024u);
  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-6);
  }

  start_divided(&divided, 100.0, 3u, 0u, 0.8f);
  render(&divided, g_alt, 96000u, 1024u);
  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-6);
  }
}

TEST(a_divided_grid_survives_a_seek)
{
  aud_click c;
  size_t at;

  start_divided(&c, 120.0, 4u, 3u, 1.0f);
  render(&c, g_buf, 96000u, 4096u);

  /* the same chunks, each seeked to first - what a looping transport does */
  memset(g_alt, 0, sizeof(g_alt));
  for (at = 0; at < 96000u; at += 4096u)
  {
    aud_click_seek(&c, at);
    aud_click_mix(&c, g_alt + at, 4096u, 1u);
  }

  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-6);
  }
}

TEST(an_out_of_range_subdivision_is_clamped_rather_than_refused)
{
  aud_click c;
  aud_click_config cfg;

  /*
   * The tempo is the thing worth refusing over - there is no sensible click at
   * 5000 BPM. A subdivision past the ceiling still has an obvious reading, so
   * it is brought back to the ceiling and the take goes ahead.
   */
  aud_click_config_defaults(&cfg, 120.0, RATE);
  cfg.subdiv = AUD_CLICK_SUBDIV_MAX + 4u;
  CHECK_EQ_INT(aud_click_init(&c, &cfg), 0);
  CHECK_EQ_INT(c.subdiv, AUD_CLICK_SUBDIV_MAX);

  /* and the burst was cut so two strikes never overlap at the tightest grid */
  CHECK(c.burst_frames <= (unsigned)c.tick_spacing);
}

TEST(a_lead_strikes_early_without_moving_the_grid)
{
  aud_click c;
  aud_click_config cfg;
  const unsigned lead = 1200u; /* 25 ms at 48 kHz, a plausible round trip */

  aud_click_config_defaults(&cfg, 120.0, RATE);
  cfg.beats_per_bar = 4u;
  cfg.gain = 1.0f;
  cfg.lead_frames = lead;
  CHECK_EQ_INT(aud_click_init(&c, &cfg), 0);

  render(&c, g_buf, 96000u, 1024u);

  /*
   * Beat n is still at frame n * 24000 of the grid - what moved is when the
   * sound is made, which is `lead` frames before that, so it comes out of the
   * output buffer as the grid reaches the beat.
   */
  for (unsigned n = 1; n < 4u; n++)
  {
    size_t onset = (size_t)n * 24000u - lead;

    /*
     * Silence up to the frame before, sin(0) on it, and sound after: which
     * between them put the strike on exactly this frame rather than somewhere
     * around it.
     */
    CHECK_EQ_DBL(g_buf[onset - 1u], 0.0, 0.0);
    CHECK_EQ_DBL(g_buf[onset], 0.0, 0.0);
    CHECK(g_buf[onset + 1u] != 0.0f);
  }

  /*
   * And the uncorrected position is not where a strike begins. Not tested as
   * silence: the burst that started `lead` frames earlier is 50 ms long, so it
   * is still ringing through here. What it is not is the start of one.
   */
  CHECK(g_buf[24000u - 1u] != 0.0f || g_buf[24000u + 1u] != 0.0f);
}

TEST(a_lead_survives_a_seek)
{
  aud_click straight;
  aud_click sought;
  aud_click_config cfg;
  size_t at;

  aud_click_config_defaults(&cfg, 137.0, RATE);
  cfg.beats_per_bar = 4u;
  cfg.gain = 1.0f;
  cfg.lead_frames = 900u;

  CHECK_EQ_INT(aud_click_init(&straight, &cfg), 0);
  CHECK_EQ_INT(aud_click_init(&sought, &cfg), 0);

  render(&straight, g_buf, 96000u, 4096u);

  /*
   * A transport that jumps has to land on the same led-forward grid, or the
   * correction would come and go with every loop wrap.
   */
  memset(g_alt, 0, sizeof(g_alt));
  for (at = 0; at < 96000u; at += 4096u)
  {
    aud_click_seek(&sought, at);
    aud_click_mix(&sought, g_alt + at, 4096u, 1u);
  }

  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-6);
  }
}

TEST(no_lead_is_the_click_where_it_always_was)
{
  aud_click plain;
  aud_click zero;
  aud_click_config cfg;

  start(&plain, 120.0, 4u, 1.0f);
  render(&plain, g_buf, 96000u, 1024u);

  aud_click_config_defaults(&cfg, 120.0, RATE);
  cfg.beats_per_bar = 4u;
  cfg.gain = 1.0f;
  cfg.lead_frames = 0u;
  CHECK_EQ_INT(aud_click_init(&zero, &cfg), 0);
  render(&zero, g_alt, 96000u, 1024u);

  for (size_t i = 0; i < 96000u; i++)
  {
    CHECK_EQ_DBL(g_alt[i], g_buf[i], 1e-9);
  }
}

int main(void)
{
  RUN(beats_land_on_the_frame_the_tempo_says);
  RUN(the_grid_does_not_drift_across_buffers);
  RUN(the_first_beat_of_the_bar_is_accented);
  RUN(a_bare_pulse_has_no_accent);
  RUN(a_beat_decays_and_stays_inside_full_scale);
  RUN(gain_scales_the_beat_and_zero_silences_it);
  RUN(every_channel_gets_the_beat_and_nothing_is_overwritten);
  RUN(reset_puts_the_grid_back_to_the_first_beat);
  RUN(seeking_lands_on_the_same_grid_as_playing_up_to_it);
  RUN(seeking_backwards_and_forwards_costs_nothing_and_stays_on_the_beat);
  RUN(unusable_tempos_are_refused);
  RUN(a_subdivision_puts_a_tick_between_the_beats);
  RUN(subdivision_ticks_are_quieter_than_the_beats_around_them);
  RUN(a_subdivision_of_one_is_the_undivided_beat);
  RUN(a_divided_grid_survives_a_seek);
  RUN(an_out_of_range_subdivision_is_clamped_rather_than_refused);
  RUN(a_lead_strikes_early_without_moving_the_grid);
  RUN(a_lead_survives_a_seek);
  RUN(no_lead_is_the_click_where_it_always_was);
  return TEST_RESULT();
}
