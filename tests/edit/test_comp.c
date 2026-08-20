/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/edit.h"

#include "edit/mix.h"
#include "edit/project.h"
#include "edit/samples.h"

#include <math.h>
#include <stdlib.h>

#define TEST_RATE 48000u

/*
 * A project of `lanes` lanes, each holding one clip `frames` long at a level of
 * its own - so which lane is being heard can be read off the samples.
 */
static void build(aud_doc *d, size_t lanes, size_t frames)
{
  aud_doc_init(d, TEST_RATE);

  for (size_t l = 0; l < lanes; l++)
  {
    aud_track *t = aud_doc_add_track(d, "pass", 1u);
    aud_samples *block = aud_samples_create(1u, frames);

    CHECK(t != NULL && block != NULL);
    if (t == NULL || block == NULL)
    {
      aud_samples_release(block);
      return;
    }

    for (size_t i = 0; i < frames; i++)
    {
      block->data[i] = (float)(l + 1u) / 10.0f;
    }

    aud_samples_index(block);
    CHECK_EQ_INT(aud_track_add(t, block, 0), 0);
    aud_samples_release(block);
  }

  aud_doc_select_all(d);
}

/* The sample lane `index` reads at `at`, through the track's own reader. */
static double heard(const aud_doc *d, size_t index, uint64_t at)
{
  float v = 0.0f;

  aud_track_read(&d->tracks[index], at, &v, 1u);
  return (double)v;
}

/* -- muting ----------------------------------------------------------------- */

TEST(a_muted_clip_reads_as_silence_without_being_taken_away)
{
  aud_doc d;

  build(&d, 1u, 1000u);

  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);
  CHECK_EQ_INT(aud_edit_mute(&d, 1), 0);
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.0, 1e-9);

  /* the audio is still there - the clip covers the frame it always did */
  CHECK_EQ_INT(aud_track_covered(&d.tracks[0], 500), 1);
  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), 1000);

  CHECK_EQ_INT(aud_edit_mute(&d, 0), 0);
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);

  aud_doc_free(&d);
}

TEST(muting_keeps_the_gain_that_was_already_there)
{
  aud_doc d;

  build(&d, 1u, 1000u);

  CHECK_EQ_INT(aud_edit_gain(&d, -6.0), 0);
  CHECK_EQ_INT(aud_edit_mute(&d, 1), 0);
  CHECK_EQ_INT(aud_edit_mute(&d, 0), 0);

  /* six decibels down, exactly as it was before the round trip */
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1 * pow(10.0, -6.0 / 20.0), 1e-5);

  aud_doc_free(&d);
}

TEST(muting_a_range_splits_at_its_edges_and_no_further)
{
  aud_doc d;

  build(&d, 1u, 1000u);
  aud_doc_select(&d, 200, 400);
  aud_doc_select_tracks(&d, 1);

  CHECK_EQ_INT(aud_edit_mute(&d, 1), 0);

  CHECK_EQ_DBL(heard(&d, 0, 100), 0.1, 1e-6);
  CHECK_EQ_DBL(heard(&d, 0, 300), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);
  CHECK_EQ_INT(d.tracks[0].count, 3);

  aud_doc_free(&d);
}

/* -- comping ---------------------------------------------------------------- */

TEST(a_comp_hears_one_pass_and_silences_the_others)
{
  aud_doc d;

  build(&d, 3u, 1000u);

  CHECK_EQ_INT(aud_edit_comp(&d, 1u), 0);

  CHECK_EQ_DBL(heard(&d, 0, 500), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.2, 1e-6);
  CHECK_EQ_DBL(heard(&d, 2, 500), 0.0, 1e-9);

  aud_doc_free(&d);
}

TEST(comping_again_changes_which_pass_is_heard_and_loses_nothing)
{
  aud_doc d;

  build(&d, 3u, 1000u);

  CHECK_EQ_INT(aud_edit_comp(&d, 1u), 0);
  CHECK_EQ_INT(aud_edit_comp(&d, 2u), 0);

  CHECK_EQ_DBL(heard(&d, 0, 500), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 2, 500), 0.3, 1e-6);

  /* and back again, to the pass the first press chose */
  CHECK_EQ_INT(aud_edit_comp(&d, 1u), 0);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.2, 1e-6);

  aud_doc_free(&d);
}

TEST(a_comp_can_be_made_a_bar_at_a_time)
{
  aud_doc d;

  build(&d, 2u, 1000u);

  aud_doc_select(&d, 0, 500);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_comp(&d, 0u), 0);

  aud_doc_select(&d, 500, 1000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_comp(&d, 1u), 0);

  CHECK_EQ_DBL(heard(&d, 0, 100), 0.1, 1e-6);
  CHECK_EQ_DBL(heard(&d, 1, 100), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 0, 700), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 1, 700), 0.2, 1e-6);

  aud_doc_free(&d);
}

TEST(a_comp_is_one_press_of_undo)
{
  aud_doc d;

  build(&d, 2u, 1000u);

  CHECK_EQ_INT(aud_edit_comp(&d, 1u), 0);
  CHECK_EQ_STR(aud_doc_undo_label(&d), "comp");
  CHECK_EQ_INT(aud_doc_undo(&d), 0);

  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.2, 1e-6);

  aud_doc_free(&d);
}

TEST(a_comp_that_would_hear_nothing_is_refused)
{
  aud_doc d;

  build(&d, 2u, 1000u);

  /* a lane that is not in the selection cannot be the one to keep */
  d.tracks[1].selected = 0;
  CHECK_EQ_INT(aud_edit_comp(&d, 1u), -1);
  CHECK_EQ_INT(aud_edit_comp(&d, 9u), -1);

  aud_doc_select_all(&d);
  aud_doc_set_cursor(&d, 0); /* a cursor is not a range */
  CHECK_EQ_INT(aud_edit_comp(&d, 0u), -1);

  CHECK(aud_doc_undo_label(&d) == NULL);
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.2, 1e-6);

  aud_doc_free(&d);
}

TEST(a_muted_pass_is_not_in_the_mix_either)
{
  aud_doc d;
  aud_mixer m;
  float out[4];

  build(&d, 2u, 1000u);
  CHECK_EQ_INT(aud_mix_init(&m, 2u), 0);

  CHECK_EQ_INT(aud_edit_comp(&d, 0u), 0);
  CHECK_EQ_INT(aud_mix_read(&m, &d, 500, out, 2u, 2u), 0);

  /* one lane's 0.1 reaching both sides, and nothing from the other */
  CHECK_EQ_DBL((double)out[0], 0.1, 1e-6);
  CHECK_EQ_DBL((double)out[1], 0.1, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

/* -- a take recorded round a loop ------------------------------------------- */

/* One lane holding `laps` laps of `length` frames, each lap at its own level. */
static void build_laps(aud_doc *d, size_t laps, size_t length, uint64_t at)
{
  aud_track *t;
  aud_samples *block;

  aud_doc_init(d, TEST_RATE);
  t = aud_doc_add_track(d, "take07.wav", 1u);
  block = aud_samples_create(1u, laps * length);

  CHECK(t != NULL && block != NULL);
  if (t == NULL || block == NULL)
  {
    aud_samples_release(block);
    return;
  }

  for (size_t i = 0; i < laps * length; i++)
  {
    block->data[i] = (float)(i / length + 1u) / 10.0f;
  }

  aud_samples_index(block);
  CHECK_EQ_INT(aud_track_add(t, block, at), 0);
  aud_samples_release(block);
}

TEST(a_take_of_three_laps_becomes_three_lanes)
{
  aud_doc d;

  build_laps(&d, 3u, 1000u, 0);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 3);
  CHECK_EQ_INT(d.count, 3);

  /* every pass starts where the loop does, and holds the lap it was */
  for (size_t i = 0; i < 3u; i++)
  {
    CHECK_EQ_INT(d.tracks[i].clips[0].start, 0);
    CHECK_EQ_INT(aud_track_end(&d.tracks[i]), 1000);
  }

  /* the audio itself is untouched, mute or no mute */
  d.tracks[0].clips[0].muted = 0;
  d.tracks[1].clips[0].muted = 0;
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.2, 1e-6);
  CHECK_EQ_DBL(heard(&d, 2, 500), 0.3, 1e-6);

  aud_doc_free(&d);
}

TEST(no_audio_is_copied_when_a_take_is_cut_into_passes)
{
  aud_doc d;
  const aud_samples *block;

  build_laps(&d, 4u, 1000u, 0);
  block = d.tracks[0].clips[0].audio;

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 4);

  /* every pass is a window onto the one block the take arrived in */
  for (size_t i = 0; i < 4u; i++)
  {
    CHECK(d.tracks[i].clips[0].audio == block);
    CHECK_EQ_INT(d.tracks[i].clips[0].offset, i * 1000u);
  }

  aud_doc_free(&d);
}

TEST(only_the_last_pass_is_heard_and_the_stack_is_left_selected)
{
  aud_doc d;

  build_laps(&d, 3u, 1000u, 0);
  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 3);

  CHECK_EQ_DBL(heard(&d, 0, 500), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 1, 500), 0.0, 1e-9);
  CHECK_EQ_DBL(heard(&d, 2, 500), 0.3, 1e-6);

  CHECK_EQ_INT(d.sel_start, 0);
  CHECK_EQ_INT(d.sel_end, 1000);
  for (size_t i = 0; i < 3u; i++)
  {
    CHECK_EQ_INT(d.tracks[i].selected, 1);
  }

  /* which means the very next press can be a comp */
  CHECK_EQ_INT(aud_edit_comp(&d, 0u), 0);
  CHECK_EQ_DBL(heard(&d, 0, 500), 0.1, 1e-6);

  aud_doc_free(&d);
}

TEST(the_passes_are_named_after_the_take_and_sit_together)
{
  aud_doc d;
  aud_track *above;

  aud_doc_init(&d, TEST_RATE);
  above = aud_doc_add_track(&d, "backing", 1u);
  CHECK(above != NULL);

  {
    aud_doc laps;

    build_laps(&laps, 2u, 1000u, 0);
    CHECK_EQ_INT(aud_track_copy(aud_doc_add_track(&d, "x", 1u), &laps.tracks[0]), 0);
    aud_doc_free(&laps);
  }

  CHECK_EQ_INT(aud_edit_take_passes(&d, 1u, 0, 1000u), 2);

  CHECK_EQ_INT(d.count, 3);
  CHECK_EQ_STR(d.tracks[0].name, "backing");
  CHECK_EQ_STR(d.tracks[1].name, "take07.wav pass 1");
  CHECK_EQ_STR(d.tracks[2].name, "take07.wav pass 2");

  aud_doc_free(&d);
}

TEST(a_take_shifted_back_by_the_latency_correction_still_cuts_at_the_laps)
{
  aud_doc d;

  /* the take begins before the loop does, which is what a correction leaves */
  build_laps(&d, 2u, 1000u, 240u);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 240u, 1000u), 2);
  CHECK_EQ_INT(d.tracks[0].clips[0].start, 240u);
  CHECK_EQ_INT(d.tracks[1].clips[0].start, 240u);

  aud_doc_free(&d);
}

TEST(a_take_that_never_came_round_again_is_left_as_it_is)
{
  aud_doc d;

  build_laps(&d, 1u, 1000u, 0);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 1);
  CHECK_EQ_INT(d.count, 1);
  CHECK_EQ_STR(d.tracks[0].name, "take07.wav");
  CHECK(aud_doc_undo_label(&d) == NULL);

  /* and a partial lap is still one pass */
  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 4000u), 1);
  CHECK_EQ_INT(d.count, 1);

  aud_doc_free(&d);
}

TEST(a_last_lap_cut_short_is_still_a_pass_of_its_own)
{
  aud_doc d;

  build_laps(&d, 2u, 1000u, 0);
  /* stop 400 frames into the third lap */
  d.tracks[0].clips[0].frames = 2400u;

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 3);
  CHECK_EQ_INT(aud_track_end(&d.tracks[2]), 400);

  aud_doc_free(&d);
}

/*
 * More laps than the project has lanes for. The last ones become passes and the
 * early ones stay where they already were, which is end to end on the lane the
 * take was recorded on - nothing is lost either way.
 */
TEST(a_project_that_runs_out_of_lanes_keeps_the_laps_it_cannot_lift)
{
  aud_doc d;
  size_t laps = AUD_DOC_MAX_TRACKS + 8u;

  build_laps(&d, laps, 100u, 0);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 100u), AUD_DOC_MAX_TRACKS);
  CHECK_EQ_INT(d.count, AUD_DOC_MAX_TRACKS);

  /* the lane the take was on still holds the laps that did not get a lane */
  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), (laps - AUD_DOC_MAX_TRACKS + 1u) * 100u);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT(d.count, 1);
  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), laps * 100u);

  aud_doc_free(&d);
}

TEST(cutting_a_take_into_passes_is_one_press_of_undo)
{
  aud_doc d;

  build_laps(&d, 3u, 1000u, 0);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 1000u), 3);
  CHECK_EQ_STR(aud_doc_undo_label(&d), "loop take");

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT(d.count, 1);
  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), 3000);
  CHECK_EQ_DBL(heard(&d, 0, 1500), 0.2, 1e-6);

  aud_doc_free(&d);
}

TEST(passes_that_do_not_describe_a_take_are_refused)
{
  aud_doc d;

  build_laps(&d, 2u, 1000u, 0);

  CHECK_EQ_INT(aud_edit_take_passes(&d, 9u, 0, 1000u), -1);
  CHECK_EQ_INT(aud_edit_take_passes(&d, 0, 0, 0), -1);
  CHECK_EQ_INT(aud_edit_take_passes(NULL, 0, 0, 1000u), -1);
  CHECK_EQ_INT(d.count, 1);

  aud_doc_free(&d);
}

int main(void)
{
  RUN(a_muted_clip_reads_as_silence_without_being_taken_away);
  RUN(muting_keeps_the_gain_that_was_already_there);
  RUN(muting_a_range_splits_at_its_edges_and_no_further);
  RUN(a_comp_hears_one_pass_and_silences_the_others);
  RUN(comping_again_changes_which_pass_is_heard_and_loses_nothing);
  RUN(a_comp_can_be_made_a_bar_at_a_time);
  RUN(a_comp_is_one_press_of_undo);
  RUN(a_comp_that_would_hear_nothing_is_refused);
  RUN(a_muted_pass_is_not_in_the_mix_either);
  RUN(a_take_of_three_laps_becomes_three_lanes);
  RUN(no_audio_is_copied_when_a_take_is_cut_into_passes);
  RUN(only_the_last_pass_is_heard_and_the_stack_is_left_selected);
  RUN(the_passes_are_named_after_the_take_and_sit_together);
  RUN(a_take_shifted_back_by_the_latency_correction_still_cuts_at_the_laps);
  RUN(a_take_that_never_came_round_again_is_left_as_it_is);
  RUN(a_last_lap_cut_short_is_still_a_pass_of_its_own);
  RUN(a_project_that_runs_out_of_lanes_keeps_the_laps_it_cannot_lift);
  RUN(cutting_a_take_into_passes_is_one_press_of_undo);
  RUN(passes_that_do_not_describe_a_take_are_refused);
  return TEST_RESULT();
}
