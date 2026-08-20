/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/limit.h"

#include "audio/limiter.h"
#include "audio/loudness.h"
#include "edit/edit.h"
#include "edit/samples.h"
#include "util/path.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR "audiaki-limit-test"
#define TEST_RATE 48000u
#define TEST_PI 3.14159265358979323846

/*
 * A project of `lanes` lanes, each holding one clip of a 400 Hz sine at its
 * own amplitude. Every lane and the whole length of it selected.
 */
static void build(aud_doc *d, size_t lanes, size_t frames, const double *amplitude)
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
      double at = (double)i / (double)TEST_RATE;

      block->data[i] = (float)(amplitude[l] * sin(2.0 * TEST_PI * 400.0 * at));
    }

    aud_samples_index(block);
    CHECK_EQ_INT(aud_track_add(t, block, 0), 0);
    aud_samples_release(block);
  }

  aud_doc_select_all(d);
}

/* The largest sample on lane `index` over the whole of it. */
static double loudest_sample(const aud_doc *d, size_t index)
{
  const aud_track *t = &d->tracks[index];
  size_t frames = (size_t)aud_track_end(t);
  float *buf = calloc(frames * t->channels, sizeof(*buf));
  double most = 0.0;

  CHECK(buf != NULL);
  if (buf == NULL)
  {
    return 0.0;
  }

  aud_track_read(t, 0, buf, frames);
  for (size_t i = 0; i < frames * t->channels; i++)
  {
    if (fabs((double)buf[i]) > most)
    {
      most = fabs((double)buf[i]);
    }
  }

  free(buf);
  return most;
}

/* -- what it does ----------------------------------------------------------- */

TEST(a_lane_over_the_ceiling_is_brought_under_it)
{
  aud_doc d;
  double amplitude[1] = {1.0};
  double reduction = 0.0;

  build(&d, 1u, TEST_RATE / 2u, amplitude);

  CHECK(aud_limit_peak_db(&d) > -1.0);
  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, &reduction, NULL), 0);

  CHECK(aud_limit_peak_db(&d) <= -1.0 + 0.05);
  CHECK(reduction > 0.5 && reduction < 3.0);

  aud_doc_free(&d);
}

TEST(the_limited_audio_knows_which_file_it_is)
{
  aud_doc d;
  double amplitude[1] = {1.0};

  build(&d, 1u, TEST_RATE / 4u, amplitude);
  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);

  /*
   * The one thing a block made by an edit must carry, or a project saved
   * afterwards has nowhere to point - see edit/project.h.
   */
  CHECK_EQ_INT(d.tracks[0].count, 1);
  CHECK(strstr(aud_samples_source(d.tracks[0].clips[0].audio), AUD_LIMIT_PREFIX) != NULL);

  aud_doc_free(&d);
}

TEST(a_lane_already_under_the_ceiling_is_not_touched_at_all)
{
  aud_doc d;
  double amplitude[1] = {0.2};
  const aud_samples *was;
  const char *why = NULL;

  build(&d, 1u, TEST_RATE / 4u, amplitude);
  was = d.tracks[0].clips[0].audio;

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, &why), -1);
  CHECK(why != NULL);

  /* the same block, not a copy of it: no file was written and no clip moved */
  CHECK(d.tracks[0].clips[0].audio == was);
  CHECK_EQ_INT(aud_doc_undo_label(&d) == NULL, 1);

  aud_doc_free(&d);
}

TEST(only_the_lanes_that_needed_it_are_rewritten)
{
  aud_doc d;
  double amplitude[3] = {0.2, 1.0, 0.1};
  const aud_samples *quiet_one;
  const aud_samples *quiet_two;

  build(&d, 3u, TEST_RATE / 4u, amplitude);
  quiet_one = d.tracks[0].clips[0].audio;
  quiet_two = d.tracks[2].clips[0].audio;

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);

  CHECK(d.tracks[0].clips[0].audio == quiet_one);
  CHECK(d.tracks[2].clips[0].audio == quiet_two);
  CHECK(loudest_sample(&d, 1u) < 0.9);

  aud_doc_free(&d);
}

TEST(limiting_several_lanes_is_one_press_of_undo)
{
  aud_doc d;
  double amplitude[2] = {1.0, 1.0};

  build(&d, 2u, TEST_RATE / 4u, amplitude);

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);
  CHECK_EQ_STR(aud_doc_undo_label(&d), AUD_LIMIT_LABEL);

  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_DBL(loudest_sample(&d, 0u), 1.0, 0.01);
  CHECK_EQ_DBL(loudest_sample(&d, 1u), 1.0, 0.01);
  CHECK_EQ_INT(aud_doc_undo(&d), -1);

  aud_doc_free(&d);
}

TEST(running_it_twice_does_nothing_the_second_time)
{
  aud_doc d;
  double amplitude[1] = {1.0};
  const aud_samples *after;

  build(&d, 1u, TEST_RATE / 4u, amplitude);

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);
  after = d.tracks[0].clips[0].audio;

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), -1);
  CHECK(d.tracks[0].clips[0].audio == after);

  aud_doc_free(&d);
}

TEST(the_clip_gain_a_normalize_left_is_folded_in_rather_than_applied_twice)
{
  aud_doc d;
  double amplitude[1] = {0.5};

  build(&d, 1u, TEST_RATE / 4u, amplitude);

  /* six decibels up puts it at full scale, the way a loudness normalize can */
  CHECK_EQ_INT(aud_edit_gain(&d, 6.0), 0);
  CHECK(loudest_sample(&d, 0u) > 0.95);

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);

  /* the gain is in the audio now, and the clip is back at unity */
  CHECK_EQ_DBL((double)d.tracks[0].clips[0].gain, 1.0, 1e-6);
  CHECK(aud_limit_peak_db(&d) <= -1.0 + 0.05);

  aud_doc_free(&d);
}

TEST(only_the_selected_range_is_limited)
{
  aud_doc d;
  double amplitude[1] = {1.0};
  size_t frames = TEST_RATE / 2u;

  build(&d, 1u, frames, amplitude);
  aud_doc_select(&d, 0, frames / 2u);

  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), 0);

  /* the second half is exactly as loud as it was */
  aud_doc_select(&d, frames / 2u, frames);
  CHECK(aud_limit_peak_db(&d) > -1.0);

  aud_doc_select(&d, 0, frames / 2u);
  CHECK(aud_limit_peak_db(&d) <= -1.0 + 0.05);

  aud_doc_free(&d);
}

/* -- what it refuses -------------------------------------------------------- */

TEST(a_press_with_nothing_selected_says_so_and_spends_no_undo)
{
  aud_doc d;
  double amplitude[1] = {1.0};
  const char *why = NULL;

  build(&d, 1u, TEST_RATE / 4u, amplitude);

  aud_doc_set_cursor(&d, 0); /* a cursor is not a range */
  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, &why), -1);
  CHECK(why != NULL);
  CHECK(aud_doc_undo_label(&d) == NULL);

  aud_doc_select_all(&d);
  aud_doc_select_tracks(&d, 0);
  CHECK_EQ_INT(aud_limit_selection(&d, -1.0, TEST_DIR, NULL, NULL), -1);
  CHECK(aud_doc_undo_label(&d) == NULL);

  aud_doc_free(&d);
}

TEST(there_is_no_peak_to_report_for_a_selection_holding_silence)
{
  aud_doc d;
  double amplitude[1] = {0.0};

  build(&d, 1u, TEST_RATE / 4u, amplitude);

  CHECK_EQ_INT(aud_loudness_measured(aud_limit_peak_db(&d)), 0);

  aud_doc_free(&d);
}

/* Every WAV the suite wrote, so it can be run twice. */
static void sweep(void)
{
  DIR *dir = opendir(TEST_DIR);
  struct dirent *entry;

  if (dir == NULL)
  {
    return;
  }

  while ((entry = readdir(dir)) != NULL)
  {
    if (strncmp(entry->d_name, AUD_LIMIT_PREFIX, strlen(AUD_LIMIT_PREFIX)) == 0)
    {
      char path[512];

      snprintf(path, sizeof(path), "%s/%s", TEST_DIR, entry->d_name);
      remove(path);
    }
  }

  closedir(dir);
}

int main(void)
{
  int rc;

  CHECK_EQ_INT(aud_path_mkdirs(TEST_DIR), 0);

  RUN(a_lane_over_the_ceiling_is_brought_under_it);
  RUN(the_limited_audio_knows_which_file_it_is);
  RUN(a_lane_already_under_the_ceiling_is_not_touched_at_all);
  RUN(only_the_lanes_that_needed_it_are_rewritten);
  RUN(limiting_several_lanes_is_one_press_of_undo);
  RUN(running_it_twice_does_nothing_the_second_time);
  RUN(the_clip_gain_a_normalize_left_is_folded_in_rather_than_applied_twice);
  RUN(only_the_selected_range_is_limited);
  RUN(a_press_with_nothing_selected_says_so_and_spends_no_undo);
  RUN(there_is_no_peak_to_report_for_a_selection_holding_silence);

  rc = TEST_RESULT();

  sweep();
  rmdir(TEST_DIR);

  return rc;
}
