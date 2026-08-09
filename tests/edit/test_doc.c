/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/doc.h"

#include <string.h>

#define RATE 48000u

TEST(a_new_project_counts_at_the_default_tempo)
{
  aud_doc d;

  aud_doc_init(&d, RATE);

  CHECK_EQ_DBL(d.tempo, AUD_DOC_DEFAULT_TEMPO, 1e-9);
  CHECK_EQ_INT(d.beats_per_bar, AUD_CLICK_DEFAULT_BEATS);

  /* 120 BPM at 48 kHz is half a second a beat, two seconds a bar of four */
  CHECK_EQ_DBL(aud_doc_beat_frames(&d), 24000.0, 1e-6);
  CHECK_EQ_DBL(aud_doc_bar_frames(&d), 96000.0, 1e-6);

  aud_doc_free(&d);
}

TEST(a_tempo_outside_what_the_click_plays_is_held_rather_than_refused)
{
  aud_doc d;

  aud_doc_init(&d, RATE);

  aud_doc_set_tempo(&d, 1e9, 4u);
  CHECK_EQ_DBL(d.tempo, AUD_CLICK_BPM_MAX, 1e-9);

  aud_doc_set_tempo(&d, -1.0, 4u);
  CHECK_EQ_DBL(d.tempo, AUD_CLICK_BPM_MIN, 1e-9);

  /* more beats to a bar than click.h will accent */
  aud_doc_set_tempo(&d, 120.0, AUD_CLICK_BEATS_MAX + 100u);
  CHECK_EQ_INT(d.beats_per_bar, AUD_CLICK_BEATS_MAX);

  /* a bare pulse is a bar of one, and a bar of one is a beat */
  aud_doc_set_tempo(&d, 120.0, 1u);
  CHECK_EQ_DBL(aud_doc_bar_frames(&d), aud_doc_beat_frames(&d), 1e-9);

  aud_doc_free(&d);
}

TEST(a_project_with_no_rate_has_no_grid)
{
  aud_doc d;

  aud_doc_init(&d, RATE);
  d.rate = 0;

  CHECK_EQ_DBL(aud_doc_beat_frames(&d), 0.0, 1e-9);
  CHECK_EQ_DBL(aud_doc_bar_frames(&d), 0.0, 1e-9);

  /* and with no grid, snapping leaves the frame exactly where it was */
  CHECK_EQ_INT(aud_doc_snap(&d, 12345u), 12345);

  aud_doc_free(&d);
}

TEST(snapping_goes_to_the_nearest_beat)
{
  aud_doc d;

  aud_doc_init(&d, RATE); /* 24000 frames to a beat */

  CHECK_EQ_INT(aud_doc_snap(&d, 0u), 0);
  CHECK_EQ_INT(aud_doc_snap(&d, 1u), 0);
  CHECK_EQ_INT(aud_doc_snap(&d, 11999u), 0);

  /* exactly half way rounds up, the way rounding does everywhere else here */
  CHECK_EQ_INT(aud_doc_snap(&d, 12000u), 24000);
  CHECK_EQ_INT(aud_doc_snap(&d, 24000u), 24000);
  CHECK_EQ_INT(aud_doc_snap(&d, 36001u), 48000);

  /* a long way in, where an accumulated grid would have drifted off the beat */
  CHECK_EQ_INT(aud_doc_snap(&d, 24000u * 5000u + 10u), 24000u * 5000u);

  aud_doc_free(&d);
}

TEST(a_tempo_that_does_not_divide_the_rate_still_snaps_to_whole_beats)
{
  aud_doc d;

  aud_doc_init(&d, 44100u);
  aud_doc_set_tempo(&d, 137.0, 4u);

  /* 44100 * 60 / 137 is not a whole number of frames */
  CHECK_EQ_DBL(aud_doc_beat_frames(&d), 2646000.0 / 137.0, 1e-6);
  CHECK(aud_doc_beat_frames(&d) != floor(aud_doc_beat_frames(&d)));

  for (unsigned beat = 1; beat <= 200u; beat++)
  {
    uint64_t onset = (uint64_t)((double)beat * aud_doc_beat_frames(&d) + 0.5);

    /* nudged either side of a beat, it comes back to that same beat */
    CHECK_EQ_INT(aud_doc_snap(&d, onset + 50u), onset);
    CHECK_EQ_INT(aud_doc_snap(&d, onset - 50u), onset);
  }

  aud_doc_free(&d);
}

TEST(the_api_tolerates_a_null_document)
{
  aud_doc_set_tempo(NULL, 120.0, 4u);
  CHECK_EQ_DBL(aud_doc_beat_frames(NULL), 0.0, 1e-9);
  CHECK_EQ_DBL(aud_doc_bar_frames(NULL), 0.0, 1e-9);
  CHECK_EQ_INT(aud_doc_snap(NULL, 99u), 99);
}

int main(void)
{
  RUN(a_new_project_counts_at_the_default_tempo);
  RUN(a_tempo_outside_what_the_click_plays_is_held_rather_than_refused);
  RUN(a_project_with_no_rate_has_no_grid);
  RUN(snapping_goes_to_the_nearest_beat);
  RUN(a_tempo_that_does_not_divide_the_rate_still_snaps_to_whole_beats);
  RUN(the_api_tolerates_a_null_document);
  return TEST_RESULT();
}
