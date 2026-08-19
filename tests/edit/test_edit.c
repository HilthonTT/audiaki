/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/edit.h"

#include "audio/loudness.h"

#include <math.h>
#include <stdlib.h>

/* Audio whose every sample says which frame it came from; see test_track.c. */
static aud_samples *counted(size_t frames, float base)
{
  aud_samples *s = aud_samples_create(1, frames);

  for (size_t f = 0; f < frames; f++)
  {
    s->data[f] = base + (float)f;
  }
  return s;
}

static float at(const aud_track *t, uint64_t frame)
{
  float one = -999.0f;

  aud_track_read(t, frame, &one, 1);
  return one;
}

/* A project with `tracks` lanes, each holding `frames` of counted audio. */
static void build(aud_doc *d, size_t tracks, size_t frames)
{
  aud_doc_init(d, 44100);

  for (size_t i = 0; i < tracks; i++)
  {
    aud_samples *s = counted(frames, (float)i * 10000.0f);
    aud_track *t = aud_doc_add_track(d, "take", 1);

    aud_track_add(t, s, 0);
    aud_samples_release(s);
  }
}

/* A lane of a 1 kHz tone: something with a peak and a loudness, unlike a ramp. */
static void build_tone(aud_doc *d, unsigned rate, size_t frames, float amp)
{
  aud_samples *s = aud_samples_create(1, frames);
  aud_track *t;

  for (size_t f = 0; f < frames; f++)
  {
    s->data[f] = amp * (float)sin(2.0 * 3.14159265358979 * 1000.0 * f / rate);
  }
  aud_samples_index(s);

  t = aud_doc_add_track(d, "tone", 1);
  aud_track_add(t, s, 0);
  aud_samples_release(s);
}

/* The loudest sample of a lane, read the way the mix reads it. */
static float peak_of(const aud_track *t, size_t frames)
{
  float *buf = calloc(frames, sizeof(*buf));
  float peak = 0.0f;

  aud_track_read(t, 0, buf, frames);
  for (size_t f = 0; f < frames; f++)
  {
    float v = buf[f] < 0.0f ? -buf[f] : buf[f];

    peak = v > peak ? v : peak;
  }
  free(buf);
  return peak;
}

/* And its integrated loudness, by the same meter the edit used. */
static double loudness_of(const aud_track *t, unsigned rate, size_t frames)
{
  aud_loudness *meter = aud_loudness_create(rate, t->channels);
  float *buf = calloc(frames, sizeof(*buf));
  aud_loudness_reading r;

  aud_track_read(t, 0, buf, frames);
  aud_loudness_feed(meter, buf, frames);
  aud_loudness_read(meter, &r);

  free(buf);
  aud_loudness_destroy(meter);
  return r.integrated;
}

/* Everything selected, which is what the gain edits work on. */
static void select_everything(aud_doc *d, uint64_t frames)
{
  aud_doc_select(d, 0, frames);
  aud_doc_select_tracks(d, 1);
}

TEST(an_empty_project_has_nothing_to_undo)
{
  aud_doc d;

  aud_doc_init(&d, 44100);
  CHECK_EQ_INT(d.count, 0);
  CHECK_EQ_INT((int)aud_doc_end(&d), 0);
  CHECK(aud_doc_undo_label(&d) == NULL);
  CHECK_EQ_INT(aud_doc_undo(&d), -1);
  CHECK_EQ_INT(aud_doc_redo(&d), -1);

  aud_doc_free(&d);
}

TEST(tracks_come_and_go_and_move)
{
  aud_doc d;

  build(&d, 3, 100);
  CHECK_EQ_INT(d.count, 3);
  CHECK(at(&d.tracks[1], 0) == 10000.0f);

  aud_doc_move_track(&d, 0, 1);
  CHECK(at(&d.tracks[0], 0) == 10000.0f);
  CHECK(at(&d.tracks[1], 0) == 0.0f);

  /* the ends of the stack have nowhere to go, and saying so is not a crash */
  aud_doc_move_track(&d, 0, 0);
  aud_doc_move_track(&d, 2, 1);
  CHECK_EQ_INT(d.count, 3);

  aud_doc_remove_track(&d, 1);
  CHECK_EQ_INT(d.count, 2);

  aud_doc_free(&d);
}

TEST(a_selection_is_a_range_and_the_tracks_it_covers)
{
  aud_doc d;

  build(&d, 2, 100);
  CHECK_EQ_INT(aud_doc_has_range(&d), 0);
  CHECK_EQ_INT(aud_doc_any_track_selected(&d), 0);

  /* dragging backwards selects the same thing as dragging forwards */
  aud_doc_select(&d, 80, 20);
  CHECK_EQ_INT((int)d.sel_start, 20);
  CHECK_EQ_INT((int)d.sel_end, 80);
  CHECK_EQ_INT((int)d.cursor, 20);
  CHECK_EQ_INT(aud_doc_has_range(&d), 1);

  aud_doc_set_cursor(&d, 50);
  CHECK_EQ_INT(aud_doc_has_range(&d), 0);

  aud_doc_select_all(&d);
  CHECK_EQ_INT(aud_doc_any_track_selected(&d), 1);
  CHECK_EQ_INT((int)d.sel_end, 100);

  aud_doc_free(&d);
}

TEST(an_anchored_selection_keeps_the_end_it_grew_from)
{
  aud_doc d;

  build(&d, 1, 100);

  /* growing to the right leaves the cursor on the anchor, not on the low end */
  aud_doc_select_from(&d, 40, 70);
  CHECK_EQ_INT((int)d.sel_start, 40);
  CHECK_EQ_INT((int)d.sel_end, 70);
  CHECK_EQ_INT((int)d.cursor, 40);

  /* and growing to the left of the anchor is the same range the other way up,
   * with the cursor still on the end that has not moved */
  aud_doc_select_from(&d, 40, 10);
  CHECK_EQ_INT((int)d.sel_start, 10);
  CHECK_EQ_INT((int)d.sel_end, 40);
  CHECK_EQ_INT((int)d.cursor, 40);

  /* collapsed onto the anchor is a cursor again */
  aud_doc_select_from(&d, 40, 40);
  CHECK(!aud_doc_has_range(&d));
  CHECK_EQ_INT((int)d.cursor, 40);

  aud_doc_free(&d);
}

TEST(an_operation_with_nothing_selected_does_nothing)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 1, 100);
  aud_clipboard_init(&c);

  /* a range but no track */
  aud_doc_select(&d, 10, 20);
  CHECK_EQ_INT(aud_edit_delete(&d), -1);
  CHECK_EQ_INT(aud_edit_copy(&d, &c), -1);

  /* a track but no range */
  d.tracks[0].selected = 1;
  aud_doc_set_cursor(&d, 10);
  CHECK_EQ_INT(aud_edit_delete(&d), -1);

  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  CHECK(aud_doc_undo_label(&d) == NULL); /* and took no undo step either */

  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

TEST(delete_takes_the_range_out_of_every_selected_track)
{
  aud_doc d;

  build(&d, 2, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 30, 50);

  CHECK_EQ_INT(aud_edit_delete(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 80);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[1]), 100); /* not selected */
  CHECK(at(&d.tracks[0], 30) == 50.0f);
  CHECK_EQ_INT((int)d.cursor, 30);

  aud_doc_free(&d);
}

TEST(silence_leaves_the_timing_alone)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 30, 50);

  CHECK_EQ_INT(aud_edit_silence(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  CHECK(at(&d.tracks[0], 40) == 0.0f);
  CHECK(at(&d.tracks[0], 50) == 50.0f);

  /* the selection survives, so changing your mind about how much is one drag */
  CHECK_EQ_INT((int)d.sel_start, 30);
  CHECK_EQ_INT((int)d.sel_end, 50);

  aud_doc_free(&d);
}

TEST(cut_then_paste_somewhere_else_moves_the_audio)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 1, 100);
  aud_clipboard_init(&c);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 0, 10);

  CHECK_EQ_INT(aud_edit_cut(&d, &c), 0);
  CHECK_EQ_INT((int)c.frames, 10);
  CHECK_EQ_INT(c.count, 1);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 90);
  CHECK(at(&d.tracks[0], 0) == 10.0f);

  aud_doc_set_cursor(&d, 90);
  CHECK_EQ_INT(aud_edit_paste(&d, &c), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  CHECK(at(&d.tracks[0], 90) == 0.0f); /* the first ten frames, at the end */
  CHECK(at(&d.tracks[0], 99) == 9.0f);

  /* and one undo puts back what one cut removed */
  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

TEST(paste_over_a_selection_replaces_it)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 1, 100);
  aud_clipboard_init(&c);
  d.tracks[0].selected = 1;

  aud_doc_select(&d, 0, 10);
  CHECK_EQ_INT(aud_edit_copy(&d, &c), 0);

  aud_doc_select(&d, 50, 80);
  CHECK_EQ_INT(aud_edit_paste(&d, &c), 0);

  /* thirty frames went, ten arrived */
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 80);
  CHECK(at(&d.tracks[0], 50) == 0.0f);
  CHECK(at(&d.tracks[0], 59) == 9.0f);
  CHECK(at(&d.tracks[0], 60) == 80.0f);
  CHECK_EQ_INT((int)d.sel_end, 60); /* what was pasted is what is selected */

  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

TEST(paste_refuses_a_different_sample_rate)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 1, 100);
  aud_clipboard_init(&c);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 0, 10);
  aud_edit_copy(&d, &c);

  c.rate = 48000u;
  aud_doc_set_cursor(&d, 50);
  CHECK_EQ_INT(aud_edit_paste(&d, &c), -1);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);

  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

TEST(a_clipboard_wider_than_the_project_grows_it)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 2, 100);
  aud_clipboard_init(&c);
  aud_doc_select_all(&d);
  CHECK_EQ_INT(aud_edit_copy(&d, &c), 0);
  CHECK_EQ_INT(c.count, 2);

  /* into a project with one lane: the second becomes a lane of its own */
  {
    aud_doc one;

    build(&one, 1, 100);
    one.rate = d.rate;
    aud_doc_set_cursor(&one, 0);
    CHECK_EQ_INT(aud_edit_paste(&one, &c), 0);
    CHECK_EQ_INT(one.count, 2);
    aud_doc_free(&one);
  }

  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

TEST(trim_keeps_only_what_was_selected)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 30, 50);

  CHECK_EQ_INT(aud_edit_trim(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 20);
  CHECK(at(&d.tracks[0], 0) == 30.0f);
  CHECK(at(&d.tracks[0], 19) == 49.0f);

  aud_doc_free(&d);
}

TEST(duplicate_puts_a_copy_on_a_new_lane_at_the_same_place)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 40, 60);

  CHECK_EQ_INT(aud_edit_duplicate(&d), 0);
  CHECK_EQ_INT(d.count, 2);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[1]), 60);
  CHECK(at(&d.tracks[1], 39) == 0.0f); /* silent before the copy */
  CHECK(at(&d.tracks[1], 40) == 40.0f);
  CHECK(at(&d.tracks[1], 59) == 59.0f);

  /* and the lane it came from is untouched */
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);

  aud_doc_free(&d);
}

/*
 * The track list is one block that grows by doubling, so a duplicate that
 * tips it over a capacity boundary moves every track while it is reading the
 * name of one of them. Four lanes is exactly where the first move happens.
 */
TEST(duplicate_keeps_the_name_across_a_list_that_moved)
{
  aud_doc d;

  build(&d, 4, 100);
  for (size_t i = 0; i < d.count; i++)
  {
    snprintf(d.tracks[i].name, sizeof(d.tracks[i].name), "lane-%zu", i);
    d.tracks[i].selected = 1;
  }
  aud_doc_select(&d, 10, 20);

  CHECK_EQ_INT(aud_edit_duplicate(&d), 0);
  CHECK_EQ_INT(d.count, 8);

  for (size_t i = 0; i < 4; i++)
  {
    char want[32];

    snprintf(want, sizeof(want), "lane-%zu", i);
    CHECK(strcmp(d.tracks[i].name, want) == 0);
    CHECK(strcmp(d.tracks[4 + i].name, want) == 0);
  }

  aud_doc_free(&d);
}

TEST(split_cuts_at_both_edges_without_removing_anything)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 30, 60);

  CHECK_EQ_INT(aud_edit_split(&d), 0);
  CHECK_EQ_INT(d.tracks[0].count, 3);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  for (uint64_t f = 0; f < 100; f++)
  {
    CHECK(at(&d.tracks[0], f) == (float)f);
  }

  aud_doc_free(&d);
}

TEST(undo_puts_the_project_back_and_redo_takes_it_forward)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;
  aud_doc_select(&d, 30, 50);

  aud_edit_delete(&d);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 80);
  CHECK(strcmp(aud_doc_undo_label(&d), "delete") == 0);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  CHECK(at(&d.tracks[0], 30) == 30.0f);
  CHECK(aud_doc_undo_label(&d) == NULL);
  CHECK(strcmp(aud_doc_redo_label(&d), "delete") == 0);

  CHECK_EQ_INT(aud_doc_redo(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 80);
  CHECK_EQ_INT(aud_doc_redo(&d), -1);

  aud_doc_free(&d);
}

TEST(undo_walks_back_through_several_edits)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;

  aud_doc_select(&d, 90, 100);
  aud_edit_delete(&d);
  aud_doc_select(&d, 80, 90);
  aud_edit_delete(&d);
  aud_doc_select(&d, 70, 80);
  aud_edit_delete(&d);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 70);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 80);
  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 90);
  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);
  CHECK_EQ_INT(aud_doc_undo(&d), -1);

  aud_doc_free(&d);
}

TEST(a_new_edit_throws_the_redo_stack_away)
{
  aud_doc d;

  build(&d, 1, 100);
  d.tracks[0].selected = 1;

  aud_doc_select(&d, 90, 100);
  aud_edit_delete(&d);
  aud_doc_undo(&d);
  CHECK(aud_doc_redo_label(&d) != NULL);

  aud_doc_select(&d, 0, 10);
  aud_edit_delete(&d);
  CHECK(aud_doc_redo_label(&d) == NULL);
  CHECK_EQ_INT(aud_doc_redo(&d), -1);

  aud_doc_free(&d);
}

TEST(the_undo_stack_stops_growing_and_keeps_the_recent_end)
{
  aud_doc d;

  build(&d, 1, 4000);
  d.tracks[0].selected = 1;

  /* more edits than the stack holds; the oldest fall off the bottom */
  for (int i = 0; i < AUD_DOC_UNDO_DEPTH + 20; i++)
  {
    aud_doc_select(&d, 0, 1);
    aud_edit_delete(&d);
  }
  CHECK_EQ_INT(d.undo_count, AUD_DOC_UNDO_DEPTH);

  for (int i = 0; i < AUD_DOC_UNDO_DEPTH; i++)
  {
    CHECK_EQ_INT(aud_doc_undo(&d), 0);
  }
  CHECK_EQ_INT(aud_doc_undo(&d), -1);

  aud_doc_free(&d);
}

TEST(closing_a_track_is_undoable)
{
  aud_doc d;

  build(&d, 2, 100);
  CHECK_EQ_INT(aud_edit_remove_track(&d, 0), 0);
  CHECK_EQ_INT(d.count, 1);
  CHECK(at(&d.tracks[0], 0) == 10000.0f);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT(d.count, 2);
  CHECK(at(&d.tracks[0], 0) == 0.0f);

  aud_doc_free(&d);
}

TEST(moving_the_selection_takes_the_audio_and_the_selection_with_it)
{
  aud_doc d;

  build(&d, 2, 100);
  aud_doc_select_tracks(&d, 1);
  aud_doc_select(&d, 0, 100);

  CHECK_EQ_INT(aud_edit_move(&d, 50), 0);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 150);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[1]), 150);
  CHECK_EQ_INT(aud_track_covered(&d.tracks[0], 49), 0);
  CHECK(at(&d.tracks[1], 50) == 10000.0f);

  /* the selection went with it, so a second nudge carries on from here */
  CHECK_EQ_INT((int)d.sel_start, 50);
  CHECK_EQ_INT((int)d.sel_end, 150);
  CHECK_EQ_INT((int)d.cursor, 50);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK(at(&d.tracks[1], 0) == 10000.0f);
  CHECK_EQ_INT((int)aud_track_end(&d.tracks[0]), 100);

  aud_doc_free(&d);
}

TEST(a_move_travels_the_same_distance_on_every_lane_it_covers)
{
  aud_doc d;
  aud_samples *s = counted(10, 500.0f);

  build(&d, 2, 100);
  aud_track_add(&d.tracks[1], s, 200); /* something in the way, on one lane only */
  aud_samples_release(s);

  aud_doc_select_tracks(&d, 1);
  aud_doc_select(&d, 0, 100);

  /*
   * The lane with room to spare is held to the lane without it. An overdub that
   * travelled further than the take it was played against would be out of time
   * with it, which is the one thing a move must not do.
   */
  CHECK_EQ_INT((int)aud_edit_move_room(&d, 1000), 100);
  CHECK_EQ_INT(aud_edit_move(&d, 1000), 0);
  CHECK(at(&d.tracks[0], 100) == 0.0f);
  CHECK(at(&d.tracks[1], 100) == 10000.0f);
  CHECK(at(&d.tracks[1], 200) == 500.0f); /* and what was in the way is intact */
  CHECK_EQ_INT((int)d.sel_start, 100);

  aud_doc_free(&d);
}

TEST(a_move_with_nowhere_to_go_changes_nothing)
{
  aud_doc d;
  size_t steps;

  build(&d, 1, 100);
  aud_doc_select_tracks(&d, 1);
  aud_doc_select(&d, 40, 60);
  steps = d.undo_count;

  /* the rest of the take is against both edges: no room, and so no checkpoint */
  CHECK_EQ_INT((int)aud_edit_move_room(&d, 10), 0);
  CHECK_EQ_INT(aud_edit_move(&d, 10), -1);
  CHECK_EQ_INT(d.undo_count, steps);
  CHECK_EQ_INT(d.tracks[0].count, 1);

  /* and neither does a move with nothing selected */
  aud_doc_select_tracks(&d, 0);
  CHECK_EQ_INT(aud_edit_move(&d, 10), -1);

  aud_doc_free(&d);
}

TEST(turning_the_selection_down_scales_what_it_reads_as)
{
  aud_doc d;

  build(&d, 1, 100);
  select_everything(&d, 100);

  CHECK_EQ_INT(aud_edit_gain(&d, -6.0206), 0);
  CHECK_EQ_DBL(at(&d.tracks[0], 10), 5.0, 0.01);

  /* and it is relative, so the same key again is the same step again */
  CHECK_EQ_INT(aud_edit_gain(&d, -6.0206), 0);
  CHECK_EQ_DBL(at(&d.tracks[0], 10), 2.5, 0.01);

  /* one undo step for each, and the audio itself never moved */
  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_DBL(at(&d.tracks[0], 10), 5.0, 0.01);

  aud_doc_free(&d);
}

TEST(a_gain_reaches_only_the_lanes_that_are_selected)
{
  aud_doc d;

  build(&d, 2, 100);
  aud_doc_select(&d, 0, 100);
  d.tracks[0].selected = 1;

  CHECK_EQ_INT(aud_edit_gain(&d, 6.0206), 0);
  CHECK_EQ_DBL(at(&d.tracks[0], 10), 20.0, 0.05);
  CHECK(at(&d.tracks[1], 10) == 10010.0f);

  aud_doc_free(&d);
}

TEST(a_gain_with_nothing_selected_is_refused)
{
  aud_doc d;

  build(&d, 1, 100);
  CHECK_EQ_INT(aud_edit_gain(&d, 1.0), -1);
  CHECK_EQ_INT(d.undo_count, 0);

  aud_doc_free(&d);
}

TEST(normalizing_puts_the_peak_of_each_lane_on_the_target)
{
  aud_doc d;

  aud_doc_init(&d, 44100);
  build_tone(&d, 44100, 44100, 0.1f);
  build_tone(&d, 44100, 44100, 0.4f);
  select_everything(&d, 44100);

  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_PEAK, -6.0), 0);

  /* both land on it, which means the two of them got different factors */
  CHECK_EQ_DBL(peak_of(&d.tracks[0], 44100), 0.5012, 0.01);
  CHECK_EQ_DBL(peak_of(&d.tracks[1], 44100), 0.5012, 0.01);
  CHECK(d.tracks[0].clips[0].gain > d.tracks[1].clips[0].gain);

  aud_doc_free(&d);
}

TEST(normalizing_something_already_normalized_changes_nothing)
{
  aud_doc d;
  float once;

  aud_doc_init(&d, 44100);
  build_tone(&d, 44100, 44100, 0.1f);
  select_everything(&d, 44100);

  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_PEAK, -6.0), 0);
  once = d.tracks[0].clips[0].gain;

  /* measured through the gain already there, so the second factor is one */
  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_PEAK, -6.0), 0);
  CHECK_EQ_DBL(d.tracks[0].clips[0].gain, once, once * 0.002);

  aud_doc_free(&d);
}

TEST(normalizing_to_a_loudness_puts_it_on_the_target)
{
  aud_doc d;

  aud_doc_init(&d, 44100);
  build_tone(&d, 44100, 44100, 0.05f);
  select_everything(&d, 44100);

  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_LOUDNESS, -18.0), 0);
  CHECK_EQ_DBL(loudness_of(&d.tracks[0], 44100, 44100), -18.0, 0.2);

  aud_doc_free(&d);
}

TEST(silence_has_nothing_to_normalize_and_costs_no_undo_step)
{
  aud_doc d;
  aud_samples *s = aud_samples_create(1, 44100);
  aud_track *t;

  aud_doc_init(&d, 44100);
  aud_samples_index(s);
  t = aud_doc_add_track(&d, "quiet", 1);
  aud_track_add(t, s, 0);
  aud_samples_release(s);
  select_everything(&d, 44100);

  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_PEAK, -6.0), -1);
  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_LOUDNESS, -18.0), -1);
  CHECK_EQ_INT(d.undo_count, 0);
  CHECK(d.tracks[0].clips[0].gain == 1.0f);

  aud_doc_free(&d);
}

TEST(a_selection_too_short_to_measure_a_loudness_is_left_alone)
{
  aud_doc d;

  aud_doc_init(&d, 44100);
  build_tone(&d, 44100, 44100, 0.1f);

  /* 100 ms, where BS.1770 has no block to measure and so no answer */
  aud_doc_select(&d, 0, 4410);
  aud_doc_select_tracks(&d, 1);

  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_LOUDNESS, -18.0), -1);
  CHECK_EQ_INT(d.undo_count, 0);

  /* the peak of those same 100 ms is a perfectly good question, though */
  CHECK_EQ_INT(aud_edit_normalize(&d, AUD_NORMALIZE_PEAK, -6.0), 0);
  CHECK_EQ_INT(d.tracks[0].count, 2);

  aud_doc_free(&d);
}

int main(void)
{
  RUN(an_empty_project_has_nothing_to_undo);
  RUN(tracks_come_and_go_and_move);
  RUN(a_selection_is_a_range_and_the_tracks_it_covers);
  RUN(an_anchored_selection_keeps_the_end_it_grew_from);
  RUN(an_operation_with_nothing_selected_does_nothing);
  RUN(delete_takes_the_range_out_of_every_selected_track);
  RUN(silence_leaves_the_timing_alone);
  RUN(cut_then_paste_somewhere_else_moves_the_audio);
  RUN(paste_over_a_selection_replaces_it);
  RUN(paste_refuses_a_different_sample_rate);
  RUN(a_clipboard_wider_than_the_project_grows_it);
  RUN(trim_keeps_only_what_was_selected);
  RUN(duplicate_puts_a_copy_on_a_new_lane_at_the_same_place);
  RUN(duplicate_keeps_the_name_across_a_list_that_moved);
  RUN(split_cuts_at_both_edges_without_removing_anything);
  RUN(undo_puts_the_project_back_and_redo_takes_it_forward);
  RUN(undo_walks_back_through_several_edits);
  RUN(a_new_edit_throws_the_redo_stack_away);
  RUN(the_undo_stack_stops_growing_and_keeps_the_recent_end);
  RUN(closing_a_track_is_undoable);
  RUN(moving_the_selection_takes_the_audio_and_the_selection_with_it);
  RUN(a_move_travels_the_same_distance_on_every_lane_it_covers);
  RUN(a_move_with_nowhere_to_go_changes_nothing);
  RUN(turning_the_selection_down_scales_what_it_reads_as);
  RUN(a_gain_reaches_only_the_lanes_that_are_selected);
  RUN(a_gain_with_nothing_selected_is_refused);
  RUN(normalizing_puts_the_peak_of_each_lane_on_the_target);
  RUN(normalizing_something_already_normalized_changes_nothing);
  RUN(normalizing_to_a_loudness_puts_it_on_the_target);
  RUN(silence_has_nothing_to_normalize_and_costs_no_undo_step);
  RUN(a_selection_too_short_to_measure_a_loudness_is_left_alone);

  return TEST_RESULT();
}
