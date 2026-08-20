/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/doc.h"

#include "edit/edit.h"
#include "edit/load.h"
#include "edit/project.h"
#include "edit/samples.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_DIR "audiaki-marker-test"
#define TEST_RATE 48000u

/* A project of `lanes` lanes, each holding one clip of `frames` frames. */
static void build(aud_doc *d, size_t lanes, size_t frames)
{
  aud_doc_init(d, TEST_RATE);

  for (size_t l = 0; l < lanes; l++)
  {
    aud_track *t = aud_doc_add_track(d, "take", 1u);
    aud_samples *block = aud_samples_create(1u, frames);

    CHECK(t != NULL && block != NULL);
    if (t == NULL || block == NULL)
    {
      aud_samples_release(block);
      return;
    }

    for (size_t i = 0; i < frames; i++)
    {
      block->data[i] = 0.25f;
    }

    aud_samples_index(block);
    CHECK_EQ_INT(aud_track_add(t, block, 0), 0);
    aud_samples_release(block);
  }
}

/* -- keeping them ----------------------------------------------------------- */

TEST(markers_come_back_in_the_order_they_sit_in)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);

  CHECK(aud_doc_mark(&d, 3000, "chorus") >= 0);
  CHECK(aud_doc_mark(&d, 1000, "verse") >= 0);
  CHECK(aud_doc_mark(&d, 2000, "bridge") >= 0);

  CHECK_EQ_INT(d.marker_count, 3);
  CHECK_EQ_INT(d.markers[0].at, 1000);
  CHECK_EQ_STR(d.markers[0].name, "verse");
  CHECK_EQ_INT(d.markers[1].at, 2000);
  CHECK_EQ_INT(d.markers[2].at, 3000);

  aud_doc_free(&d);
}

TEST(a_second_marker_at_one_frame_renames_the_first)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);

  CHECK_EQ_INT(aud_doc_mark(&d, 1000, "verse"), 0);
  CHECK_EQ_INT(aud_doc_mark(&d, 1000, "chorus"), 0);

  CHECK_EQ_INT(d.marker_count, 1);
  CHECK_EQ_STR(d.markers[0].name, "chorus");

  aud_doc_free(&d);
}

TEST(a_marker_may_be_only_a_place)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);

  CHECK_EQ_INT(aud_doc_mark(&d, 500, NULL), 0);
  CHECK_EQ_STR(d.markers[0].name, "");

  aud_doc_free(&d);
}

TEST(there_is_a_ceiling_on_how_many_a_project_may_hold)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);

  for (size_t i = 0; i < AUD_DOC_MAX_MARKERS; i++)
  {
    CHECK(aud_doc_mark(&d, (uint64_t)i * 100u, "x") >= 0);
  }
  CHECK_EQ_INT(d.marker_count, AUD_DOC_MAX_MARKERS);
  CHECK_EQ_INT(aud_doc_mark(&d, 9999999, "one too many"), -1);
  CHECK_EQ_INT(d.marker_count, AUD_DOC_MAX_MARKERS);

  aud_doc_free(&d);
}

TEST(taking_one_away_leaves_the_rest_in_order)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);
  CHECK(aud_doc_mark(&d, 100, "a") >= 0);
  CHECK(aud_doc_mark(&d, 200, "b") >= 0);
  CHECK(aud_doc_mark(&d, 300, "c") >= 0);

  aud_doc_unmark(&d, 1);
  CHECK_EQ_INT(d.marker_count, 2);
  CHECK_EQ_STR(d.markers[0].name, "a");
  CHECK_EQ_STR(d.markers[1].name, "c");

  aud_doc_unmark(&d, 99); /* past the end changes nothing */
  CHECK_EQ_INT(d.marker_count, 2);

  aud_doc_clear_markers(&d);
  CHECK_EQ_INT(d.marker_count, 0);

  aud_doc_free(&d);
}

/* -- finding them ----------------------------------------------------------- */

TEST(a_pointer_lands_on_the_nearest_marker_within_reach)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);
  CHECK(aud_doc_mark(&d, 1000, "a") >= 0);
  CHECK(aud_doc_mark(&d, 5000, "b") >= 0);

  CHECK_EQ_INT(aud_doc_marker_near(&d, 1040, 100), 0);
  CHECK_EQ_INT(aud_doc_marker_near(&d, 4950, 100), 1);
  CHECK_EQ_INT(aud_doc_marker_near(&d, 3000, 100), -1);

  /* exactly between two: the earlier one, so the answer does not wobble */
  CHECK_EQ_INT(aud_doc_marker_near(&d, 3000, 5000), 0);

  CHECK_EQ_INT(aud_doc_marker_at(&d, 5000), 1);
  CHECK_EQ_INT(aud_doc_marker_at(&d, 5001), -1);

  aud_doc_free(&d);
}

TEST(stepping_always_moves_and_stops_at_the_ends)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);
  CHECK(aud_doc_mark(&d, 1000, "a") >= 0);
  CHECK(aud_doc_mark(&d, 2000, "b") >= 0);

  CHECK_EQ_INT(aud_doc_marker_step(&d, 0, 0), 1000);
  CHECK_EQ_INT(aud_doc_marker_step(&d, 1000, 0), 2000); /* off it, not onto it */
  CHECK_EQ_INT(aud_doc_marker_step(&d, 2000, 0), 2000); /* nothing that way */

  CHECK_EQ_INT(aud_doc_marker_step(&d, 2000, 1), 1000);
  CHECK_EQ_INT(aud_doc_marker_step(&d, 1000, 1), 1000);

  aud_doc_free(&d);
}

TEST(a_project_with_no_markers_has_nowhere_to_step)
{
  aud_doc d;

  aud_doc_init(&d, TEST_RATE);
  CHECK_EQ_INT(aud_doc_marker_step(&d, 500, 0), 500);
  CHECK_EQ_INT(aud_doc_marker_step(&d, 500, 1), 500);
  CHECK_EQ_INT(aud_doc_marker_near(&d, 500, 1000), -1);
  aud_doc_free(&d);
}

/* -- what an edit does to them ---------------------------------------------- */

TEST(a_delete_across_every_lane_takes_the_ruler_with_it)
{
  aud_doc d;

  build(&d, 2u, 10000u);
  CHECK(aud_doc_mark(&d, 1000, "before") >= 0);
  CHECK(aud_doc_mark(&d, 3000, "inside") >= 0);
  CHECK(aud_doc_mark(&d, 6000, "after") >= 0);

  aud_doc_select(&d, 2000, 4000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_delete(&d), 0);

  /* the one inside the range went with the audio it was pointing at */
  CHECK_EQ_INT(d.marker_count, 2);
  CHECK_EQ_INT(d.markers[0].at, 1000);
  CHECK_EQ_STR(d.markers[0].name, "before");
  CHECK_EQ_INT(d.markers[1].at, 4000);
  CHECK_EQ_STR(d.markers[1].name, "after");

  aud_doc_free(&d);
}

TEST(undoing_that_delete_puts_the_ruler_back)
{
  aud_doc d;

  build(&d, 1u, 10000u);
  CHECK(aud_doc_mark(&d, 3000, "inside") >= 0);
  CHECK(aud_doc_mark(&d, 6000, "after") >= 0);

  aud_doc_select(&d, 2000, 4000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_delete(&d), 0);
  CHECK_EQ_INT(d.marker_count, 1);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_INT(d.marker_count, 2);
  CHECK_EQ_INT(d.markers[0].at, 3000);
  CHECK_EQ_INT(d.markers[1].at, 6000);

  CHECK_EQ_INT(aud_doc_redo(&d), 0);
  CHECK_EQ_INT(d.marker_count, 1);
  CHECK_EQ_INT(d.markers[0].at, 4000);

  aud_doc_free(&d);
}

TEST(a_delete_on_one_lane_of_two_leaves_the_ruler_alone)
{
  aud_doc d;

  build(&d, 2u, 10000u);
  CHECK(aud_doc_mark(&d, 6000, "after") >= 0);

  aud_doc_select(&d, 2000, 4000);
  aud_doc_select_tracks(&d, 0);
  d.tracks[0].selected = 1;
  CHECK_EQ_INT(aud_edit_delete(&d), 0);

  /*
   * The second lane still reaches where it reached, so the project is no
   * shorter and the marker is still over the same moment of it.
   */
  CHECK_EQ_INT(d.marker_count, 1);
  CHECK_EQ_INT(d.markers[0].at, 6000);

  aud_doc_free(&d);
}

TEST(a_trim_moves_the_ruler_back_with_the_head_it_removed)
{
  aud_doc d;

  build(&d, 1u, 10000u);
  CHECK(aud_doc_mark(&d, 500, "cut away") >= 0);
  CHECK(aud_doc_mark(&d, 3000, "kept") >= 0);
  CHECK(aud_doc_mark(&d, 9000, "cut away too") >= 0);

  aud_doc_select(&d, 2000, 6000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_trim(&d), 0);

  CHECK_EQ_INT(d.marker_count, 1);
  CHECK_EQ_INT(d.markers[0].at, 1000);
  CHECK_EQ_STR(d.markers[0].name, "kept");

  aud_doc_free(&d);
}

TEST(a_paste_opens_room_on_the_ruler_as_well)
{
  aud_doc d;
  aud_clipboard c;

  build(&d, 1u, 10000u);
  aud_clipboard_init(&c);

  aud_doc_select(&d, 0, 1000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_copy(&d, &c), 0);

  CHECK(aud_doc_mark(&d, 5000, "later") >= 0);

  aud_doc_set_cursor(&d, 2000);
  aud_doc_select_tracks(&d, 1);
  CHECK_EQ_INT(aud_edit_paste(&d, &c), 0);

  CHECK_EQ_INT(d.marker_count, 1);
  CHECK_EQ_INT(d.markers[0].at, 6000);

  aud_clipboard_clear(&c);
  aud_doc_free(&d);
}

/* -- keeping them across a save --------------------------------------------- */

TEST(markers_survive_being_written_and_read_back)
{
  aud_doc d;
  aud_doc back;
  char project[512];
  char wav[512];
  const char *why = NULL;

  build(&d, 1u, 4800u);

  /* a real file for the project to refer to, or the save is refused */
  CHECK_EQ_INT(aud_edit_write_block(d.tracks[0].clips[0].audio, TEST_RATE, TEST_DIR,
                                    "marked", wav, sizeof(wav), &why),
               0);
  CHECK_EQ_INT(aud_samples_set_source(d.tracks[0].clips[0].audio, wav), 0);

  CHECK(aud_doc_mark(&d, 1000, "verse two") >= 0);
  CHECK(aud_doc_mark(&d, 2400, "") >= 0);

  snprintf(project, sizeof(project), "%s/marked%s", TEST_DIR, AUD_PROJECT_EXT);
  CHECK_EQ_INT(aud_project_save(&d, project, &why), 0);

  aud_doc_init(&back, 0);
  CHECK_EQ_INT(aud_project_load(&back, project, &why), 0);

  CHECK_EQ_INT(back.marker_count, 2);
  CHECK_EQ_INT(back.markers[0].at, 1000);
  CHECK_EQ_STR(back.markers[0].name, "verse two");
  CHECK_EQ_INT(back.markers[1].at, 2400);
  CHECK_EQ_STR(back.markers[1].name, "");

  remove(project);
  remove(wav);
  aud_doc_free(&back);
  aud_doc_free(&d);
}

/*
 * A hand-edited project, which is a thing the format invites: a line whose
 * frame will not parse is a label lost rather than a session refused.
 */
TEST(a_marker_line_that_makes_no_sense_is_stepped_over)
{
  aud_doc back;
  char project[512];
  const char *why = NULL;
  FILE *f;

  snprintf(project, sizeof(project), "%s/hand%s", TEST_DIR, AUD_PROJECT_EXT);
  f = fopen(project, "wb");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }

  fprintf(f, "%s %d\n", AUD_PROJECT_MAGIC, AUD_PROJECT_VERSION);
  fprintf(f, "rate %u\n", TEST_RATE);
  fprintf(f, "marker 1000 verse two\n");
  fprintf(f, "marker not-a-number ignored\n");
  fprintf(f, "marker 2400\n");
  fclose(f);

  aud_doc_init(&back, 0);
  CHECK_EQ_INT(aud_project_load(&back, project, &why), 0);
  CHECK_EQ_INT(back.marker_count, 2);
  CHECK_EQ_STR(back.markers[0].name, "verse two");
  CHECK_EQ_STR(back.markers[1].name, "");

  remove(project);
  aud_doc_free(&back);
}

int main(void)
{
  int rc;

  CHECK_EQ_INT(aud_path_mkdirs(TEST_DIR), 0);

  RUN(markers_come_back_in_the_order_they_sit_in);
  RUN(a_second_marker_at_one_frame_renames_the_first);
  RUN(a_marker_may_be_only_a_place);
  RUN(there_is_a_ceiling_on_how_many_a_project_may_hold);
  RUN(taking_one_away_leaves_the_rest_in_order);
  RUN(a_pointer_lands_on_the_nearest_marker_within_reach);
  RUN(stepping_always_moves_and_stops_at_the_ends);
  RUN(a_project_with_no_markers_has_nowhere_to_step);
  RUN(a_delete_across_every_lane_takes_the_ruler_with_it);
  RUN(undoing_that_delete_puts_the_ruler_back);
  RUN(a_delete_on_one_lane_of_two_leaves_the_ruler_alone);
  RUN(a_trim_moves_the_ruler_back_with_the_head_it_removed);
  RUN(a_paste_opens_room_on_the_ruler_as_well);
  RUN(markers_survive_being_written_and_read_back);
  RUN(a_marker_line_that_makes_no_sense_is_stepped_over);

  rc = TEST_RESULT();

  rmdir(TEST_DIR);
  return rc;
}
