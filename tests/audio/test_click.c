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
  RUN(unusable_tempos_are_refused);
  return TEST_RESULT();
}
