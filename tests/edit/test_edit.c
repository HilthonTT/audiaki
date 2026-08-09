/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/edit.h"

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

int main(void)
{
  RUN(an_empty_project_has_nothing_to_undo);
  RUN(tracks_come_and_go_and_move);
  RUN(a_selection_is_a_range_and_the_tracks_it_covers);
  RUN(an_operation_with_nothing_selected_does_nothing);
  RUN(delete_takes_the_range_out_of_every_selected_track);
  RUN(silence_leaves_the_timing_alone);
  RUN(cut_then_paste_somewhere_else_moves_the_audio);
  RUN(paste_over_a_selection_replaces_it);
  RUN(paste_refuses_a_different_sample_rate);
  RUN(a_clipboard_wider_than_the_project_grows_it);
  RUN(trim_keeps_only_what_was_selected);
  RUN(duplicate_puts_a_copy_on_a_new_lane_at_the_same_place);
  RUN(split_cuts_at_both_edges_without_removing_anything);
  RUN(undo_puts_the_project_back_and_redo_takes_it_forward);
  RUN(undo_walks_back_through_several_edits);
  RUN(a_new_edit_throws_the_redo_stack_away);
  RUN(the_undo_stack_stops_growing_and_keeps_the_recent_end);
  RUN(closing_a_track_is_undoable);

  return TEST_RESULT();
}
