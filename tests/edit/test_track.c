/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/track.h"

#include <stdlib.h>

/*
 * Audio whose every sample says which frame it came from, so a test can assert
 * what a clip is showing rather than only how long it is. Frame n reads n.
 */
static aud_samples *counted(size_t frames, float base)
{
  aud_samples *s = aud_samples_create(1, frames);

  for (size_t f = 0; f < frames; f++)
  {
    s->data[f] = base + (float)f;
  }
  return s;
}

/* What the timeline holds at `frame`, or a sentinel where it holds nothing. */
static float at(const aud_track *t, uint64_t frame)
{
  float one = -999.0f;

  aud_track_read(t, frame, &one, 1);
  return one;
}

/* Non-zero when the clip list is still sorted, non-overlapping and non-empty. */
static int well_formed(const aud_track *t)
{
  for (size_t i = 0; i < t->count; i++)
  {
    if (t->clips[i].frames == 0)
    {
      return 0;
    }
    if (t->clips[i].offset + t->clips[i].frames > t->clips[i].audio->frames)
    {
      return 0;
    }
    if (i > 0 && t->clips[i].start < t->clips[i - 1].start + t->clips[i - 1].frames)
    {
      return 0;
    }
  }
  return 1;
}

TEST(a_track_starts_empty)
{
  aud_track t;

  CHECK_EQ_INT(aud_track_init(&t, "Take 1", 2), 0);
  CHECK_EQ_INT(t.count, 0);
  CHECK_EQ_INT(t.channels, 2);
  CHECK_EQ_INT((int)aud_track_end(&t), 0);
  CHECK(t.gain == 1.0f);
  CHECK_EQ_INT(aud_track_covered(&t, 0), 0);

  aud_track_free(&t);
}

TEST(audio_placed_on_the_timeline_reads_back_where_it_was_put)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  CHECK_EQ_INT(aud_track_add(&t, s, 50), 0);
  aud_samples_release(s);

  CHECK_EQ_INT((int)aud_track_end(&t), 150);
  CHECK(at(&t, 0) == 0.0f);   /* the gap before it is silence */
  CHECK(at(&t, 50) == 0.0f);  /* ...and so is frame 0 of the audio */
  CHECK(at(&t, 60) == 10.0f); /* ten frames in */
  CHECK(at(&t, 200) == 0.0f); /* past the end */

  CHECK_EQ_INT(aud_track_covered(&t, 49), 0);
  CHECK_EQ_INT(aud_track_covered(&t, 50), 1);
  CHECK_EQ_INT(aud_track_covered(&t, 149), 1);
  CHECK_EQ_INT(aud_track_covered(&t, 150), 0);

  aud_track_free(&t);
}

TEST(overlapping_audio_is_refused)
{
  aud_track t;
  aud_samples *a = counted(100, 0.0f);
  aud_samples *b = counted(100, 1000.0f);

  aud_track_init(&t, "t", 1);
  CHECK_EQ_INT(aud_track_add(&t, a, 0), 0);
  CHECK_EQ_INT(aud_track_add(&t, b, 50), -1);
  CHECK_EQ_INT(aud_track_add(&t, b, 100), 0); /* exactly after is not overlap */
  CHECK_EQ_INT(t.count, 2);
  CHECK(well_formed(&t));

  aud_samples_release(a);
  aud_samples_release(b);
  aud_track_free(&t);
}

TEST(splitting_makes_two_clips_over_one_block)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  CHECK_EQ_INT(aud_track_split(&t, 40), 0);
  CHECK_EQ_INT(t.count, 2);
  CHECK(t.clips[0].audio == t.clips[1].audio); /* no audio was copied */
  CHECK_EQ_INT((int)t.clips[0].frames, 40);
  CHECK_EQ_INT((int)t.clips[1].frames, 60);
  CHECK_EQ_INT((int)t.clips[1].offset, 40);
  CHECK(well_formed(&t));

  /* and it still sounds like one take */
  CHECK(at(&t, 39) == 39.0f);
  CHECK(at(&t, 40) == 40.0f);

  /* a split on a boundary, in a gap or past the end changes nothing */
  CHECK_EQ_INT(aud_track_split(&t, 40), 0);
  CHECK_EQ_INT(aud_track_split(&t, 500), 0);
  CHECK_EQ_INT(t.count, 2);

  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(deleting_with_ripple_shortens_the_track)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  CHECK_EQ_INT(aud_track_delete(&t, 30, 50, 1), 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 80);
  CHECK(well_formed(&t));

  /* what was at 50 is now at 30, and the seam is closed */
  CHECK(at(&t, 29) == 29.0f);
  CHECK(at(&t, 30) == 50.0f);
  CHECK(at(&t, 79) == 99.0f);

  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(deleting_without_ripple_leaves_a_hole)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  CHECK_EQ_INT(aud_track_delete(&t, 30, 50, 0), 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 100); /* everything else stayed put */
  CHECK_EQ_INT(t.count, 2);
  CHECK(well_formed(&t));

  CHECK(at(&t, 29) == 29.0f);
  CHECK(at(&t, 30) == 0.0f); /* the hole */
  CHECK(at(&t, 49) == 0.0f);
  CHECK(at(&t, 50) == 50.0f);
  CHECK_EQ_INT(aud_track_covered(&t, 40), 0);

  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(deleting_a_whole_clip_removes_it)
{
  aud_track t;
  aud_samples *a = counted(50, 0.0f);
  aud_samples *b = counted(50, 1000.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, a, 0);
  aud_track_add(&t, b, 100);

  CHECK_EQ_INT(aud_track_delete(&t, 100, 150, 0), 0);
  CHECK_EQ_INT(t.count, 1);
  CHECK_EQ_INT((int)aud_track_end(&t), 50);

  aud_samples_release(a);
  aud_samples_release(b);
  aud_track_free(&t);
}

TEST(a_gap_pushes_everything_after_it_along)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  CHECK_EQ_INT(aud_track_insert_gap(&t, 40, 25), 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 125);
  CHECK(well_formed(&t));

  CHECK(at(&t, 39) == 39.0f);
  CHECK(at(&t, 50) == 0.0f);
  CHECK(at(&t, 65) == 40.0f);

  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(extracting_gives_the_range_starting_at_zero)
{
  aud_track t;
  aud_track piece;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 20);

  CHECK_EQ_INT(aud_track_extract(&t, 50, 70, &piece), 0);
  CHECK_EQ_INT((int)aud_track_end(&piece), 20);
  CHECK(at(&piece, 0) == 30.0f); /* 50 on the timeline is frame 30 of the audio */
  CHECK(at(&piece, 19) == 49.0f);
  CHECK(well_formed(&piece));

  /* and the track it came from is untouched */
  CHECK_EQ_INT((int)aud_track_end(&t), 120);

  aud_track_free(&piece);
  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(extracting_a_range_with_a_hole_keeps_the_hole)
{
  aud_track t;
  aud_track piece;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_track_delete(&t, 40, 60, 0);

  CHECK_EQ_INT(aud_track_extract(&t, 30, 70, &piece), 0);
  CHECK_EQ_INT(piece.count, 2);
  CHECK(at(&piece, 0) == 30.0f);
  CHECK(at(&piece, 15) == 0.0f); /* the hole came with it */
  CHECK(at(&piece, 35) == 65.0f);

  aud_track_free(&piece);
  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(pasting_opens_room_and_puts_the_clips_in_it)
{
  aud_track t;
  aud_track piece;
  aud_samples *a = counted(100, 0.0f);
  aud_samples *b = counted(10, 1000.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, a, 0);

  aud_track_init(&piece, "p", 1);
  aud_track_add(&piece, b, 0);

  CHECK_EQ_INT(aud_track_paste(&t, 50, &piece), 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 110);
  CHECK(well_formed(&t));

  CHECK(at(&t, 49) == 49.0f);
  CHECK(at(&t, 50) == 1000.0f); /* what was pasted */
  CHECK(at(&t, 59) == 1009.0f);
  CHECK(at(&t, 60) == 50.0f); /* what it pushed along */

  aud_track_free(&piece);
  aud_samples_release(a);
  aud_samples_release(b);
  aud_track_free(&t);
}

TEST(cut_and_paste_back_is_the_take_it_started_as)
{
  aud_track t;
  aud_track piece;
  aud_samples *s = counted(200, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  CHECK_EQ_INT(aud_track_extract(&t, 40, 90, &piece), 0);
  CHECK_EQ_INT(aud_track_delete(&t, 40, 90, 1), 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 150);
  CHECK_EQ_INT(aud_track_paste(&t, 40, &piece), 0);

  CHECK_EQ_INT((int)aud_track_end(&t), 200);
  for (uint64_t f = 0; f < 200; f++)
  {
    CHECK(at(&t, f) == (float)f);
  }

  /* and it is one clip again, because tidy noticed the seam was not one */
  CHECK_EQ_INT(t.count, 1);

  aud_track_free(&piece);
  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(tidy_joins_a_split_back_up_but_leaves_a_real_boundary)
{
  aud_track t;
  aud_samples *a = counted(100, 0.0f);
  aud_samples *b = counted(100, 1000.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, a, 0);
  aud_track_add(&t, b, 100);

  aud_track_split(&t, 50);
  CHECK_EQ_INT(t.count, 3);

  aud_track_tidy(&t);
  CHECK_EQ_INT(t.count, 2); /* the split closed; the two takes stayed apart */
  CHECK(at(&t, 99) == 99.0f);
  CHECK(at(&t, 100) == 1000.0f);

  aud_samples_release(a);
  aud_samples_release(b);
  aud_track_free(&t);
}

TEST(a_copied_track_shares_its_audio_and_is_independent)
{
  aud_track t;
  aud_track copy;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  CHECK_EQ_INT(s->refs, 2); /* the test's own, and the clip's */

  CHECK_EQ_INT(aud_track_copy(&copy, &t), 0);
  CHECK_EQ_INT(s->refs, 3); /* shared, not duplicated */

  /* editing one does not touch the other, which is what undo relies on */
  aud_track_delete(&t, 0, 50, 1);
  CHECK_EQ_INT((int)aud_track_end(&t), 50);
  CHECK_EQ_INT((int)aud_track_end(&copy), 100);
  CHECK(at(&copy, 10) == 10.0f);

  aud_track_free(&copy);
  aud_track_free(&t);
  CHECK_EQ_INT(s->refs, 1);
  aud_samples_release(s);
}

TEST(a_range_over_a_hole_takes_in_the_silence)
{
  aud_track t;
  aud_samples *s = aud_samples_create(1, 100);
  aud_peak got;

  for (size_t f = 0; f < 100; f++)
  {
    s->data[f] = 0.5f; /* every sample well above zero */
  }

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);

  aud_track_range(&t, 0, 0, 100, &got);
  CHECK_EQ_DBL(got.min, 0.5, 1e-6);
  CHECK_EQ_DBL(got.max, 0.5, 1e-6);
  CHECK_EQ_DBL(got.rms, 0.5, 1e-6);

  aud_track_delete(&t, 40, 60, 0);
  aud_track_range(&t, 0, 0, 100, &got);
  CHECK_EQ_DBL(got.min, 0.0, 1e-6); /* the hole is part of what that span holds */
  CHECK_EQ_DBL(got.max, 0.5, 1e-6);
  /* eighty frames at 0.5 and twenty at nothing */
  CHECK_EQ_DBL(got.rms, sqrt(0.25 * 0.8), 1e-6);

  /* and past the end of everything is silence too */
  aud_track_range(&t, 0, 200, 300, &got);
  CHECK_EQ_DBL(got.min, 0.0, 1e-6);
  CHECK_EQ_DBL(got.max, 0.0, 1e-6);

  aud_samples_release(s);
  aud_track_free(&t);
}

TEST(a_mono_block_on_a_stereo_track_does_not_read_past_its_frame)
{
  aud_track t;
  aud_samples *s = counted(50, 0.0f);
  float out[8];

  aud_track_init(&t, "t", 2);
  aud_track_add(&t, s, 0);

  aud_track_read(&t, 10, out, 4);
  CHECK(out[0] == 10.0f);
  CHECK(out[1] == 0.0f); /* the channel the block does not have stays silent */
  CHECK(out[2] == 11.0f);
  CHECK(out[3] == 0.0f);

  aud_samples_release(s);
  aud_track_free(&t);
}

/*
 * The recording index is a position in the clip list, and the window lets a
 * lane be edited while a take is running down it. So every list operation has
 * to carry the index with it, and a list that has lost the clip it named has to
 * end the take rather than leave the next captured period pointing at whatever
 * moved into the slot.
 */
TEST(an_edit_ahead_of_a_take_leaves_the_take_where_it_is)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);
  float buf[8] = {7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f};

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT(aud_track_record_begin(&t, 200, 64), 0);
  CHECK(aud_track_recording(&t));
  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 8), 8);

  /* a split of the clip before it inserts a clip underneath the take's index */
  CHECK_EQ_INT(aud_track_split(&t, 50), 0);
  CHECK_EQ_INT(t.count, 3);
  CHECK(aud_track_recording(&t));

  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 8), 8);
  aud_track_record_end(&t);

  /* the take is still its own clip, holding both pushes and nothing else */
  CHECK(well_formed(&t));
  CHECK_EQ_INT((int)aud_track_end(&t), 216);
  CHECK(at(&t, 200) == 7.0f);
  CHECK(at(&t, 215) == 7.0f);
  CHECK(at(&t, 49) == 49.0f); /* the split clip is untouched */

  aud_track_free(&t);
}

/*
 * The same lane, but the cut lands inside the take rather than ahead of it.
 *
 * A block that is still being recorded into grows at its end, so the frames
 * still to arrive belong to the half of the split that owns that end. The take
 * has to move to it, and its length has to be measured from its own offset -
 * otherwise the next captured period stretches the left half back over the
 * right one and the list stops being sorted and non-overlapping.
 */
TEST(splitting_a_take_that_is_still_running_leaves_both_halves_whole)
{
  aud_track t;
  float first[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  float second[8] = {2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f};

  aud_track_init(&t, "t", 1);

  CHECK_EQ_INT(aud_track_record_begin(&t, 100, 64), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, first, 8), 8);

  /* halfway through what has arrived so far */
  CHECK_EQ_INT(aud_track_split(&t, 104), 0);
  CHECK_EQ_INT(t.count, 2);
  CHECK(aud_track_recording(&t));

  CHECK_EQ_INT((int)aud_track_record_push(&t, second, 8), 8);
  aud_track_record_end(&t);

  CHECK(well_formed(&t));
  CHECK_EQ_INT(t.count, 2);
  CHECK_EQ_INT((int)aud_track_end(&t), 116);

  /* the first push either side of the cut, then the second after it */
  CHECK(at(&t, 100) == 1.0f);
  CHECK(at(&t, 103) == 1.0f);
  CHECK(at(&t, 104) == 1.0f);
  CHECK(at(&t, 107) == 1.0f);
  CHECK(at(&t, 108) == 2.0f);
  CHECK(at(&t, 115) == 2.0f);

  aud_track_free(&t);
}

TEST(an_interrupted_take_carries_on_in_the_same_clip)
{
  aud_track t;
  float first[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float second[4] = {2.0f, 2.0f, 2.0f, 2.0f};

  aud_track_init(&t, "t", 1);

  /* a take that the device was pulled out of four frames in */
  CHECK_EQ_INT(aud_track_record_begin(&t, 100, 64), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, first, 4), 4);
  aud_track_record_end(&t);
  CHECK_EQ_INT(t.count, 1);

  /* the device came back, and the rest of it goes into the same clip */
  CHECK_EQ_INT(aud_track_record_continue(&t, 104), 0);
  CHECK(aud_track_recording(&t));
  CHECK_EQ_INT((int)aud_track_record_push(&t, second, 4), 4);
  aud_track_record_end(&t);

  /*
   * One clip, not two touching ones. Two would both be stamped with the one
   * file the halves now share and both claim to start at the beginning of it.
   */
  CHECK_EQ_INT(t.count, 1);
  CHECK(well_formed(&t));
  CHECK_EQ_INT((int)aud_track_end(&t), 108);
  CHECK(at(&t, 100) == 1.0f);
  CHECK(at(&t, 103) == 1.0f);
  CHECK(at(&t, 104) == 2.0f);
  CHECK(at(&t, 107) == 2.0f);

  aud_track_free(&t);
}

TEST(carrying_a_take_on_is_refused_when_it_would_not_be_the_same_take)
{
  aud_track t;
  float buf[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  aud_track_init(&t, "t", 1);

  /* nothing recorded yet: there is no tail to carry on */
  CHECK_EQ_INT(aud_track_record_continue(&t, 0), -1);

  CHECK_EQ_INT(aud_track_record_begin(&t, 100, 64), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 4), 4);

  /* a take is already open */
  CHECK_EQ_INT(aud_track_record_continue(&t, 104), -1);
  aud_track_record_end(&t);

  /* not where the clip actually ends: something was edited in the gap */
  CHECK_EQ_INT(aud_track_record_continue(&t, 103), -1);
  CHECK_EQ_INT(aud_track_record_continue(&t, 105), -1);

  /* and the right frame still works, so the refusals above were about the gap */
  CHECK_EQ_INT(aud_track_record_continue(&t, 104), 0);
  aud_track_record_end(&t);

  aud_track_free(&t);
}

TEST(carrying_a_take_on_is_refused_when_the_audio_is_shared)
{
  aud_track t;
  float buf[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

  aud_track_init(&t, "t", 1);
  CHECK_EQ_INT(aud_track_record_begin(&t, 0, 64), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 8), 8);
  aud_track_record_end(&t);

  /*
   * A split leaves two clips reading one block. Growing it would change what
   * the other one plays, so this has to decline and let the caller open a
   * second file the way it always did.
   */
  CHECK_EQ_INT(aud_track_split(&t, 4), 0);
  CHECK_EQ_INT(t.count, 2);
  CHECK_EQ_INT(aud_track_record_continue(&t, 8), -1);

  aud_track_free(&t);
}

TEST(carrying_a_take_on_is_refused_when_the_block_is_not_the_lanes_width)
{
  aud_track t;
  aud_samples *mono = counted(100, 0.0f);
  float stereo[512] = {0};

  /*
   * A lane whose tail holds a block of another shape, which a paste across
   * lanes or a project file naming its own channel count both leave behind.
   * The take about to arrive is two channels wide and the block has room for
   * one, so carrying on into it would write past what was reserved.
   */
  aud_track_init(&t, "t", 2);
  CHECK_EQ_INT(aud_track_place(&t, mono, 0, 100, 0, 0, 0), 0);
  aud_samples_release(mono); /* the clip is the only owner now */

  CHECK_EQ_INT(aud_track_record_continue(&t, 100), -1);
  CHECK(!aud_track_recording(&t));

  /* and the push itself declines, whatever managed to arm it */
  t.recording = 0;
  CHECK_EQ_INT((int)aud_track_record_push(&t, stereo, 256), 0);
  CHECK_EQ_INT((int)t.clips[0].audio->frames, 100);

  t.recording = -1;
  aud_track_free(&t);
}

TEST(a_clip_that_would_run_off_the_end_of_the_timeline_is_refused)
{
  aud_track t;
  aud_samples *a = counted(8, 0.0f);

  aud_track_init(&t, "t", 1);

  /* a start this far along leaves nowhere for eight frames to end */
  CHECK_EQ_INT(aud_track_place(&t, a, 0, 8, UINT64_MAX - 4u, 0, 0), -1);
  CHECK_EQ_INT((int)t.count, 0);

  /* one that does fit is still placed */
  CHECK_EQ_INT(aud_track_place(&t, a, 0, 8, UINT64_MAX - 8u, 0, 0), 0);
  CHECK_EQ_INT((int)t.count, 1);
  CHECK(well_formed(&t));

  aud_samples_release(a);
  aud_track_free(&t);
}

TEST(deleting_the_clip_a_take_is_going_into_ends_the_take)
{
  aud_track t;
  float buf[8] = {7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f};

  aud_track_init(&t, "t", 1);

  CHECK_EQ_INT(aud_track_record_begin(&t, 0, 64), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 8), 8);
  CHECK_EQ_INT(t.count, 1);

  /* the user selects the growing take and presses Delete */
  CHECK_EQ_INT(aud_track_delete(&t, 0, 8, 1), 0);
  CHECK_EQ_INT(t.count, 0);
  CHECK(!aud_track_recording(&t));

  /* whatever the engine had still in flight has nowhere to go, and says so
   * rather than being written through a clip index that no longer exists */
  CHECK_EQ_INT((int)aud_track_record_push(&t, buf, 8), 0);
  aud_track_record_end(&t);
  CHECK_EQ_INT(t.count, 0);

  aud_track_free(&t);
}

/* -- fades ------------------------------------------------------------------ */

/* A track of one clip of `frames` frames, every sample full scale. */
static void flat(aud_track *t, size_t frames)
{
  aud_samples *s = aud_samples_create(1, frames);

  for (size_t f = 0; f < frames; f++)
  {
    s->data[f] = 1.0f;
  }
  aud_samples_index(s);

  aud_track_init(t, "t", 1);
  aud_track_add(t, s, 0);
  aud_samples_release(s);
}

TEST(a_fade_in_ramps_from_silence_to_full)
{
  aud_track t;

  flat(&t, 200);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 0, 100), 0);

  CHECK(at(&t, 0) == 0.0f); /* the first frame is silent, not nearly silent */
  CHECK_EQ_DBL(at(&t, 25), 0.25, 1e-6);
  CHECK_EQ_DBL(at(&t, 50), 0.50, 1e-6);
  CHECK_EQ_DBL(at(&t, 99), 0.99, 1e-6);
  CHECK(at(&t, 100) == 1.0f); /* past the ramp, untouched */
  CHECK(at(&t, 199) == 1.0f);

  aud_track_free(&t);
}

TEST(a_fade_out_ramps_to_silence_at_the_last_frame)
{
  aud_track t;

  flat(&t, 200);
  CHECK_EQ_INT(aud_track_fade_out_at(&t, 200, 100), 0);

  CHECK(at(&t, 99) == 1.0f);
  CHECK_EQ_DBL(at(&t, 150), 0.49, 1e-6);
  CHECK(at(&t, 199) == 0.0f); /* the last frame is silence */

  aud_track_free(&t);
}

TEST(a_fade_shows_in_the_waveform_as_well_as_the_audio)
{
  aud_track t;
  aud_peak head;
  aud_peak body;

  flat(&t, 2000);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 0, 1000), 0);

  /* the summary a waveform column is drawn from follows the ramp down */
  aud_track_range(&t, 0, 0, 100, &head);
  aud_track_range(&t, 0, 1500, 1600, &body);
  CHECK(head.max < 0.2f);
  CHECK(body.max > 0.99f);
  CHECK(head.rms < body.rms);

  aud_track_free(&t);
}

TEST(fades_survive_a_split_at_each_end_they_belong_to)
{
  aud_track t;

  flat(&t, 400);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 0, 50), 0);
  CHECK_EQ_INT(aud_track_fade_out_at(&t, 400, 50), 0);

  /* a cut in the middle, clear of both ramps */
  CHECK_EQ_INT(aud_track_split(&t, 200), 0);
  CHECK_EQ_INT(t.count, 2);
  CHECK_EQ_INT((int)t.clips[0].fade_in, 50);
  CHECK_EQ_INT((int)t.clips[0].fade_out, 0);
  CHECK_EQ_INT((int)t.clips[1].fade_in, 0);
  CHECK_EQ_INT((int)t.clips[1].fade_out, 50);

  /* and the audio still ramps at both outer ends */
  CHECK(at(&t, 0) == 0.0f);
  CHECK(at(&t, 399) == 0.0f);
  CHECK(at(&t, 200) == 1.0f);

  aud_track_free(&t);
}

TEST(tidy_will_not_join_two_clips_across_a_fade)
{
  aud_track t;

  flat(&t, 400);
  CHECK_EQ_INT(aud_track_split(&t, 200), 0);
  CHECK_EQ_INT(aud_track_fade_out_at(&t, 200, 40), 0);

  /* without the fade these two would merge back into one; with it they must
   * not, or the ramp would vanish along with the boundary it sits on */
  aud_track_tidy(&t);
  CHECK_EQ_INT(t.count, 2);
  CHECK(at(&t, 199) == 0.0f);

  /* take the fade off and they join again */
  CHECK_EQ_INT(aud_track_fade_out_at(&t, 200, 0), 0);
  aud_track_tidy(&t);
  CHECK_EQ_INT(t.count, 1);
  CHECK(at(&t, 199) == 1.0f);

  aud_track_free(&t);
}

TEST(a_fade_longer_than_its_clip_is_held_to_it)
{
  aud_track t;

  flat(&t, 100);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 0, 100000), 0);
  CHECK_EQ_INT((int)t.clips[0].fade_in, 100);

  /* deleting into the clip shortens it, and the ramp comes with it */
  CHECK_EQ_INT(aud_track_delete(&t, 60, 100, 0), 0);
  CHECK_EQ_INT((int)t.clips[0].frames, 60);
  CHECK(t.clips[0].fade_in <= t.clips[0].frames);

  aud_track_free(&t);
}

TEST(a_fade_is_asked_for_at_an_edge_and_refused_anywhere_else)
{
  aud_track t;

  flat(&t, 200);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 100, 20), -1); /* mid-clip, no boundary */
  CHECK_EQ_INT(aud_track_fade_out_at(&t, 100, 20), -1);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 500, 20), -1); /* past the end */
  CHECK(at(&t, 0) == 1.0f);                            /* nothing happened */

  aud_track_free(&t);
}

/* -- clip edges -------------------------------------------------------------- */

TEST(a_gain_scales_what_the_clip_reads_as)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT(aud_track_gain_scale(&t, 0, 100, 2.0f), 0);
  CHECK(at(&t, 10) == 20.0f);
  CHECK(t.clips[0].gain == 2.0f);

  /* multiplied rather than set, so leaning on the key compounds */
  CHECK_EQ_INT(aud_track_gain_scale(&t, 0, 100, 0.25f), 0);
  CHECK(at(&t, 10) == 5.0f);
  CHECK(t.clips[0].gain == 0.5f);

  aud_track_free(&t);
}

TEST(a_gain_over_part_of_a_take_splits_it_at_both_edges)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT(aud_track_gain_scale(&t, 30, 60, 2.0f), 0);
  CHECK_EQ_INT(t.count, 3);
  CHECK(well_formed(&t));

  CHECK(at(&t, 29) == 29.0f); /* before it, as it was */
  CHECK(at(&t, 30) == 60.0f); /* inside it, twice as loud */
  CHECK(at(&t, 59) == 118.0f);
  CHECK(at(&t, 60) == 60.0f); /* and after it, as it was again */

  aud_track_free(&t);
}

TEST(a_gain_shows_in_the_waveform_as_well_as_the_audio)
{
  aud_track t;
  aud_samples *s = aud_samples_create(1, 100);
  aud_peak p;

  for (size_t f = 0; f < 100; f++)
  {
    s->data[f] = 0.25f;
  }
  aud_samples_index(s);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  aud_track_gain_scale(&t, 0, 100, 2.0f);
  aud_track_range(&t, 0, 0, 100, &p);
  CHECK_EQ_DBL(p.max, 0.5, 1e-6);
  CHECK_EQ_DBL(p.rms, 0.5, 1e-6);

  aud_track_free(&t);
}

TEST(a_gain_is_held_to_what_the_model_can_say)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  aud_track_gain_scale(&t, 0, 100, 1000.0f);
  CHECK(t.clips[0].gain == AUD_CLIP_GAIN_MAX);

  /* and it cannot be asked below silence either */
  aud_track_gain_scale(&t, 0, 100, -1.0f);
  CHECK(t.clips[0].gain == 0.0f);
  CHECK(at(&t, 10) == 0.0f);

  aud_track_free(&t);
}

TEST(a_gain_survives_a_split_and_an_extract)
{
  aud_track t;
  aud_track piece;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  aud_track_gain_scale(&t, 0, 100, 2.0f);
  aud_track_split(&t, 50);
  CHECK_EQ_INT(t.count, 2);
  CHECK(t.clips[0].gain == 2.0f);
  CHECK(t.clips[1].gain == 2.0f); /* both halves are the same piece, turned the same */

  CHECK_EQ_INT(aud_track_extract(&t, 20, 40, &piece), 0);
  CHECK(piece.clips[0].gain == 2.0f);
  CHECK(at(&piece, 5) == 50.0f);
  aud_track_free(&piece);

  aud_track_free(&t);
}

TEST(tidy_will_not_join_two_clips_turned_differently)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  aud_track_gain_scale(&t, 50, 100, 2.0f);
  CHECK_EQ_INT(t.count, 2);

  aud_track_tidy(&t);
  CHECK_EQ_INT(t.count, 2); /* the boundary is a real one now */
  CHECK(at(&t, 60) == 120.0f);

  /* put it back and the two halves are one take again */
  aud_track_gain_scale(&t, 50, 100, 0.5f);
  aud_track_tidy(&t);
  CHECK_EQ_INT(t.count, 1);
  CHECK(at(&t, 60) == 60.0f);

  aud_track_free(&t);
}

TEST(a_gain_over_a_range_with_no_audio_leaves_the_clips_alone)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  /* past the end: no clip to turn, and no split left behind for having tried */
  CHECK_EQ_INT(aud_track_gain_scale(&t, 200, 300, 2.0f), -1);
  CHECK_EQ_INT(t.count, 1);
  CHECK(at(&t, 10) == 10.0f);

  aud_track_free(&t);
}

TEST(the_edges_of_a_track_are_where_its_clips_begin_and_end)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 50);  /* [50, 150) */
  aud_track_add(&t, s, 300); /* [300, 400) */
  aud_samples_release(s);

  /* forwards, from before everything and from each edge in turn */
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 0), 50);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 50), 150);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 149), 150);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 150), 300);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 300), 400);
  /* past the last edge there is nothing, and it says so by not moving */
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 400), 400);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 9999), 9999);

  /* and backwards */
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 9999), 400);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 400), 300);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 300), 150);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 151), 150);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 150), 50);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 50), 50);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 0), 0);

  aud_track_free(&t);
}

TEST(a_split_adds_an_edge_to_step_to)
{
  aud_track t;
  aud_samples *s = counted(100, 0.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT((int)aud_track_edge_after(&t, 0), 100); /* one clip, two edges */
  CHECK_EQ_INT(aud_track_split(&t, 40), 0);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 0), 40); /* now three */
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 40), 100);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 100), 40);

  aud_track_free(&t);
}

TEST(an_empty_track_has_nowhere_to_step_to)
{
  aud_track t;

  aud_track_init(&t, "t", 1);
  CHECK_EQ_INT((int)aud_track_edge_after(&t, 500), 500);
  CHECK_EQ_INT((int)aud_track_edge_before(&t, 500), 500);
  CHECK_EQ_INT((int)aud_track_edge_after(NULL, 7), 7);
  CHECK_EQ_INT((int)aud_track_edge_before(NULL, 7), 7);
  aud_track_free(&t);
}

TEST(moving_a_range_takes_the_audio_with_it)
{
  aud_track t;
  aud_samples *s = counted(20, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 100);
  aud_samples_release(s);

  /* a take on a lane of its own, two hundred frames later */
  CHECK_EQ_INT(aud_track_move(&t, 100, 120, 200), 0);
  CHECK(well_formed(&t));
  CHECK_EQ_INT(t.count, 1);
  CHECK(at(&t, 100) == 0.0f); /* nothing left where it was */
  CHECK(at(&t, 300) == 1.0f);
  CHECK(at(&t, 319) == 20.0f);
  CHECK_EQ_INT((int)aud_track_end(&t), 320);

  /* and back the way it came, which is as far as frame 0 and no further */
  CHECK_EQ_INT(aud_track_move(&t, 300, 320, -1000), -1);
  CHECK_EQ_INT(aud_track_move(&t, 300, 320, -200), 0);
  CHECK(at(&t, 100) == 1.0f);

  aud_track_free(&t);
}

TEST(a_move_leaves_a_hole_where_it_came_from)
{
  aud_track t;
  aud_samples *s = counted(20, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  /* the tail of a take, moved off the end of it: the take is shorter and the
   * piece is out on its own with silence in between */
  CHECK_EQ_INT(aud_track_move(&t, 10, 20, 100), 0);
  CHECK(well_formed(&t));
  CHECK_EQ_INT(t.count, 2);
  CHECK(at(&t, 9) == 10.0f);
  CHECK(at(&t, 10) == 0.0f); /* the hole it left */
  CHECK(at(&t, 109) == 0.0f);
  CHECK(at(&t, 110) == 11.0f);
  CHECK(at(&t, 119) == 20.0f);
  CHECK_EQ_INT((int)aud_track_end(&t), 120);

  aud_track_free(&t);
}

TEST(a_move_back_to_where_it_was_joins_the_take_up_again)
{
  aud_track t;
  aud_samples *s = counted(50, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT(aud_track_move(&t, 10, 50, 500), 0);
  CHECK_EQ_INT(t.count, 2);
  CHECK_EQ_INT(aud_track_move(&t, 510, 550, -500), 0);
  CHECK(well_formed(&t));

  /* one clip again, because tidy joins what reads straight through */
  CHECK_EQ_INT(t.count, 1);
  CHECK(at(&t, 0) == 1.0f);
  CHECK(at(&t, 49) == 50.0f);

  aud_track_free(&t);
}

TEST(a_move_stops_against_what_is_already_there)
{
  aud_track t;
  aud_samples *one = counted(10, 1.0f);
  aud_samples *two = counted(10, 100.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, one, 0);
  aud_track_add(&t, two, 40);
  aud_samples_release(one);
  aud_samples_release(two);

  /* thirty frames of clear ground between the two, and no more than that */
  CHECK_EQ_INT((int)aud_track_move_room(&t, 0, 10, 100), 30);
  CHECK_EQ_INT((int)aud_track_move_room(&t, 0, 10, 12), 12);
  CHECK_EQ_INT((int)aud_track_move_room(&t, 0, 10, -5), 0);
  CHECK_EQ_INT((int)aud_track_move_room(&t, 40, 50, -100), -30);

  /* and a move further than the room is refused rather than overwriting */
  CHECK_EQ_INT(aud_track_move(&t, 0, 10, 100), -1);
  CHECK(at(&t, 40) == 100.0f);
  CHECK_EQ_INT(aud_track_move(&t, 0, 10, 30), 0);
  CHECK(well_formed(&t));
  CHECK(at(&t, 30) == 1.0f);
  CHECK(at(&t, 40) == 100.0f);

  aud_track_free(&t);
}

TEST(the_middle_of_a_take_has_nowhere_to_move_to)
{
  aud_track t;
  aud_samples *s = counted(100, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  /* the rest of the take is against both edges of the range */
  CHECK_EQ_INT((int)aud_track_move_room(&t, 40, 50, 10), 0);
  CHECK_EQ_INT((int)aud_track_move_room(&t, 40, 50, -10), 0);
  CHECK_EQ_INT(aud_track_move(&t, 40, 50, 10), -1);
  CHECK_EQ_INT(t.count, 1);

  aud_track_free(&t);
}

TEST(a_move_cannot_go_back_past_the_start_of_the_timeline)
{
  aud_track t;
  aud_samples *s = counted(10, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 20);
  aud_samples_release(s);

  CHECK_EQ_INT((int)aud_track_move_room(&t, 20, 30, -1000), -20);
  CHECK_EQ_INT(aud_track_move(&t, 20, 30, -20), 0);
  CHECK(well_formed(&t));
  CHECK(at(&t, 0) == 1.0f);
  CHECK_EQ_INT((int)aud_track_end(&t), 10);

  aud_track_free(&t);
}

TEST(a_move_keeps_the_fades_on_the_piece_that_moved)
{
  aud_track t;
  aud_samples *s = counted(100, 1.0f);

  aud_track_init(&t, "t", 1);
  aud_track_add(&t, s, 0);
  aud_samples_release(s);

  CHECK_EQ_INT(aud_track_split(&t, 20), 0);
  CHECK_EQ_INT(aud_track_fade_in_at(&t, 20, 4), 0);
  CHECK_EQ_INT(aud_track_move(&t, 20, 100, 400), 0);
  CHECK(well_formed(&t));

  CHECK(at(&t, 420) == 0.0f); /* still ramping out of silence at its new head */
  CHECK(at(&t, 424) == 25.0f);

  aud_track_free(&t);
}

TEST(a_take_that_is_still_running_is_not_moved)
{
  aud_track t;
  float block[4] = {1.0f, 2.0f, 3.0f, 4.0f};

  aud_track_init(&t, "t", 1);
  CHECK_EQ_INT(aud_track_record_begin(&t, 0, 4), 0);
  CHECK_EQ_INT((int)aud_track_record_push(&t, block, 4), 4);

  CHECK_EQ_INT(aud_track_move(&t, 0, 4, 100), -1);
  CHECK(at(&t, 0) == 1.0f);

  aud_track_record_end(&t);
  CHECK_EQ_INT(aud_track_move(&t, 0, 4, 100), 0);
  CHECK(at(&t, 100) == 1.0f);

  aud_track_free(&t);
}

int main(void)
{
  RUN(a_track_starts_empty);
  RUN(audio_placed_on_the_timeline_reads_back_where_it_was_put);
  RUN(overlapping_audio_is_refused);
  RUN(splitting_makes_two_clips_over_one_block);
  RUN(deleting_with_ripple_shortens_the_track);
  RUN(deleting_without_ripple_leaves_a_hole);
  RUN(deleting_a_whole_clip_removes_it);
  RUN(a_gap_pushes_everything_after_it_along);
  RUN(extracting_gives_the_range_starting_at_zero);
  RUN(extracting_a_range_with_a_hole_keeps_the_hole);
  RUN(pasting_opens_room_and_puts_the_clips_in_it);
  RUN(cut_and_paste_back_is_the_take_it_started_as);
  RUN(tidy_joins_a_split_back_up_but_leaves_a_real_boundary);
  RUN(a_copied_track_shares_its_audio_and_is_independent);
  RUN(a_range_over_a_hole_takes_in_the_silence);
  RUN(a_mono_block_on_a_stereo_track_does_not_read_past_its_frame);
  RUN(an_edit_ahead_of_a_take_leaves_the_take_where_it_is);
  RUN(deleting_the_clip_a_take_is_going_into_ends_the_take);
  RUN(a_fade_in_ramps_from_silence_to_full);
  RUN(a_fade_out_ramps_to_silence_at_the_last_frame);
  RUN(a_fade_shows_in_the_waveform_as_well_as_the_audio);
  RUN(fades_survive_a_split_at_each_end_they_belong_to);
  RUN(tidy_will_not_join_two_clips_across_a_fade);
  RUN(a_fade_longer_than_its_clip_is_held_to_it);
  RUN(a_fade_is_asked_for_at_an_edge_and_refused_anywhere_else);
  RUN(a_gain_scales_what_the_clip_reads_as);
  RUN(a_gain_over_part_of_a_take_splits_it_at_both_edges);
  RUN(a_gain_shows_in_the_waveform_as_well_as_the_audio);
  RUN(a_gain_is_held_to_what_the_model_can_say);
  RUN(a_gain_survives_a_split_and_an_extract);
  RUN(tidy_will_not_join_two_clips_turned_differently);
  RUN(a_gain_over_a_range_with_no_audio_leaves_the_clips_alone);
  RUN(the_edges_of_a_track_are_where_its_clips_begin_and_end);
  RUN(a_split_adds_an_edge_to_step_to);
  RUN(an_empty_track_has_nowhere_to_step_to);
  RUN(splitting_a_take_that_is_still_running_leaves_both_halves_whole);
  RUN(an_interrupted_take_carries_on_in_the_same_clip);
  RUN(carrying_a_take_on_is_refused_when_it_would_not_be_the_same_take);
  RUN(carrying_a_take_on_is_refused_when_the_audio_is_shared);
  RUN(carrying_a_take_on_is_refused_when_the_block_is_not_the_lanes_width);
  RUN(a_clip_that_would_run_off_the_end_of_the_timeline_is_refused);
  RUN(moving_a_range_takes_the_audio_with_it);
  RUN(a_move_leaves_a_hole_where_it_came_from);
  RUN(a_move_back_to_where_it_was_joins_the_take_up_again);
  RUN(a_move_stops_against_what_is_already_there);
  RUN(the_middle_of_a_take_has_nowhere_to_move_to);
  RUN(a_move_cannot_go_back_past_the_start_of_the_timeline);
  RUN(a_move_keeps_the_fades_on_the_piece_that_moved);
  RUN(a_take_that_is_still_running_is_not_moved);

  return TEST_RESULT();
}
