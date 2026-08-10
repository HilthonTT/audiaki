/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/repair.h"

#include "edit/project.h"
#include "edit/samples.h"
#include "util/path.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR "audiaki-repair-test"
#define TEST_RATE 48000u
#define TEST_FFT 1024u

#define AUD_TEST_PI 3.14159265358979323846

/* A track of `frames` frames holding a hum at 300 Hz and a note at 2 kHz. */
static void build(aud_doc *d, size_t frames, unsigned channels)
{
  aud_samples *block;
  aud_track *t;

  aud_doc_init(d, TEST_RATE);
  t = aud_doc_add_track(d, "bass", channels);
  CHECK(t != NULL);

  block = aud_samples_create(channels, frames);
  CHECK(block != NULL);
  if (block == NULL || t == NULL)
  {
    return;
  }

  for (size_t i = 0; i < frames; i++)
  {
    double at = (double)i / (double)TEST_RATE;
    float v = (float)(0.3 * sin(2.0 * AUD_TEST_PI * 300.0 * at) +
                      0.3 * sin(2.0 * AUD_TEST_PI * 2000.0 * at));

    for (unsigned c = 0; c < channels; c++)
    {
      block->data[i * channels + c] = v;
    }
  }

  aud_samples_index(block);
  CHECK_EQ_INT(aud_track_add(t, block, 0), 0);
  aud_samples_release(block);
}

/* Root mean square of channel 0 over timeline frames [from, to). */
static double rms(const aud_doc *d, uint64_t from, uint64_t to)
{
  const aud_track *t = &d->tracks[0];
  size_t frames = (size_t)(to - from);
  float *buf = calloc(frames * t->channels, sizeof(*buf));
  double total = 0.0;

  CHECK(buf != NULL);
  if (buf == NULL)
  {
    return 0.0;
  }

  aud_track_read(t, from, buf, frames);
  for (size_t i = 0; i < frames; i++)
  {
    double v = (double)buf[i * t->channels];
    total += v * v;
  }

  free(buf);
  return frames > 0 ? sqrt(total / (double)frames) : 0.0;
}

/* Take out the 300 Hz hum and nothing else. */
static aud_spectral *humless(void)
{
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);

  CHECK(s != NULL);
  if (s != NULL)
  {
    aud_spectral_notch(s, 300.0, 200.0, 1u, 0.0f);
  }
  return s;
}

TEST(reading_a_range_finds_what_is_in_it)
{
  aud_doc d;
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);

  build(&d, TEST_FFT * 40u, 1u);
  CHECK(s != NULL);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  CHECK_EQ_INT(aud_repair_read(&d, 0, 0, TEST_FFT * 40u, s), 0);
  CHECK(aud_spectral_has_reading(s));

  /*
   * Spread across the range rather than taken from the start of it, and capped,
   * so a long take costs the same reading as a short one.
   */
  CHECK(aud_spectral_windows(s) <= AUD_REPAIR_MAX_WINDOWS);
  CHECK(aud_spectral_windows(s) > 1);

  {
    const float *mean = aud_spectral_mean(s);

    CHECK(aud_spectral_db(mean[aud_spectral_bin_at(s, 300.0)]) > -20.0f);
    CHECK(aud_spectral_db(mean[aud_spectral_bin_at(s, 2000.0)]) > -20.0f);
    CHECK(aud_spectral_db(mean[aud_spectral_bin_at(s, 8000.0)]) < -60.0f);
  }

  /* a range shorter than one window still reads, zero-padded */
  CHECK_EQ_INT(aud_repair_read(&d, 0, 0, 200, s), 0);
  CHECK_EQ_INT(aud_spectral_windows(s), 1);

  /* and one that describes no audio at all does not */
  CHECK_EQ_INT(aud_repair_read(&d, 0, 100, 100, s), -1);
  CHECK_EQ_INT(aud_repair_read(&d, 9, 0, 500, s), -1);

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(applying_takes_the_hum_out_of_the_track)
{
  aud_doc d;
  aud_spectral *s = humless();
  const size_t frames = TEST_FFT * 40u;
  double before;
  double after;

  build(&d, frames, 1u);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  before = rms(&d, TEST_FFT, frames - TEST_FFT);
  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, s, TEST_DIR, NULL), 0);
  after = rms(&d, TEST_FFT, frames - TEST_FFT);

  /* two equal tones went in and one came out: about 1/sqrt(2) of the energy */
  CHECK(before > after * 1.3);
  CHECK_EQ_DBL(after, 0.3 / sqrt(2.0), 0.02);

  /* the lane is one clip over one block again, whatever it was before */
  CHECK_EQ_INT(d.tracks[0].count, 1);
  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), frames);

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(the_repaired_block_knows_where_it_was_written)
{
  aud_doc d;
  aud_spectral *s = humless();
  const size_t frames = TEST_FFT * 8u;
  char project[256];

  build(&d, frames, 1u);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, s, TEST_DIR, NULL), 0);

  {
    const aud_samples *block = d.tracks[0].clips[0].audio;
    const char *source = aud_samples_source(block);
    FILE *f;

    CHECK(strstr(source, AUD_REPAIR_PREFIX) != NULL);

    /* and the file is really there, which is what makes the session saveable */
    f = fopen(source, "rb");
    CHECK(f != NULL);
    if (f != NULL)
    {
      fclose(f);
    }
  }

  /*
   * The point of writing it at all: a project is a list of which parts of which
   * files sit where, and a block from nowhere cannot be written down.
   */
  snprintf(project, sizeof(project), "%s/repaired%s", TEST_DIR, AUD_PROJECT_EXT);
  CHECK_EQ_INT(aud_project_save(&d, project, NULL), 0);
  remove(project);

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(a_repair_is_one_press_of_undo)
{
  aud_doc d;
  aud_spectral *s = humless();
  const size_t frames = TEST_FFT * 8u;
  double before;

  build(&d, frames, 1u);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  before = rms(&d, TEST_FFT, frames - TEST_FFT);

  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, s, TEST_DIR, NULL), 0);
  CHECK(rms(&d, TEST_FFT, frames - TEST_FFT) < before * 0.9);

  CHECK_EQ_STR(aud_doc_undo_label(&d), AUD_REPAIR_LABEL);
  CHECK_EQ_INT(aud_doc_undo(&d), 0);
  CHECK_EQ_DBL(rms(&d, TEST_FFT, frames - TEST_FFT), before, 1e-6);

  /* and forward again, which is why the written file is not deleted */
  CHECK_EQ_INT(aud_doc_redo(&d), 0);
  CHECK(rms(&d, TEST_FFT, frames - TEST_FFT) < before * 0.9);

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(only_the_selected_range_is_touched)
{
  aud_doc d;
  aud_spectral *s = humless();
  const size_t frames = TEST_FFT * 40u;
  uint64_t from = TEST_FFT * 16u;
  uint64_t to = TEST_FFT * 24u;
  double outside_before;
  double outside_after;

  build(&d, frames, 1u);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  outside_before = rms(&d, 0, TEST_FFT * 8u);
  CHECK_EQ_INT(aud_repair_apply(&d, 0, from, to, s, TEST_DIR, NULL), 0);
  outside_after = rms(&d, 0, TEST_FFT * 8u);

  /* audio away from the range is the audio that was there */
  CHECK_EQ_DBL(outside_after, outside_before, 1e-6);

  /* inside it, the hum is gone */
  CHECK_EQ_DBL(rms(&d, from + TEST_FFT, to - TEST_FFT), 0.3 / sqrt(2.0), 0.03);

  /*
   * And the join is not a seam. The run-up either side means the samples right
   * at the boundary are filtered against the audio that really is next to
   * them, so nothing dips or jumps across it.
   */
  {
    double just_inside = rms(&d, from, from + 400);
    double well_inside = rms(&d, from + TEST_FFT, from + TEST_FFT + 400);

    CHECK_EQ_DBL(just_inside, well_inside, well_inside * 0.15);
  }

  CHECK_EQ_INT(aud_track_end(&d.tracks[0]), frames);

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(a_stereo_take_keeps_both_sides)
{
  aud_doc d;
  aud_spectral *s = humless();
  const size_t frames = TEST_FFT * 8u;

  build(&d, frames, 2u);
  if (s == NULL)
  {
    aud_doc_free(&d);
    return;
  }

  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, s, TEST_DIR, NULL), 0);
  CHECK_EQ_INT(d.tracks[0].clips[0].audio->channels, 2);

  {
    const aud_samples *block = d.tracks[0].clips[0].audio;
    double left = 0.0;
    double right = 0.0;

    for (size_t i = TEST_FFT; i < frames - TEST_FFT; i++)
    {
      left += (double)block->data[i * 2u] * (double)block->data[i * 2u];
      right += (double)block->data[i * 2u + 1u] * (double)block->data[i * 2u + 1u];
    }

    CHECK_EQ_DBL(left, right, left * 1e-6);
    CHECK(left > 0.0);
  }

  aud_spectral_destroy(s);
  aud_doc_free(&d);
}

TEST(a_repair_that_cannot_be_done_changes_nothing)
{
  aud_doc d;
  aud_spectral *s = aud_spectral_create(TEST_RATE, TEST_FFT);
  aud_spectral *wrong_rate = aud_spectral_create(TEST_RATE * 2u, TEST_FFT);
  const size_t frames = TEST_FFT * 4u;
  const char *why;

  build(&d, frames, 1u);
  CHECK(s != NULL && wrong_rate != NULL);
  if (s == NULL || wrong_rate == NULL)
  {
    aud_spectral_destroy(s);
    aud_spectral_destroy(wrong_rate);
    aud_doc_free(&d);
    return;
  }

  /* a flat curve and no profile: there is nothing to do, and it says so */
  why = NULL;
  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, s, TEST_DIR, &why), -1);
  CHECK(why != NULL);

  aud_spectral_notch(s, 300.0, 200.0, 1u, 0.0f);
  aud_spectral_notch(wrong_rate, 300.0, 200.0, 1u, 0.0f);

  CHECK_EQ_INT(aud_repair_apply(&d, 9, 0, frames, s, TEST_DIR, NULL), -1);
  CHECK_EQ_INT(aud_repair_apply(&d, 0, frames, frames, s, TEST_DIR, NULL), -1);
  CHECK_EQ_INT(aud_repair_apply(&d, 0, 0, frames, wrong_rate, TEST_DIR, NULL), -1);

  /* none of that took a checkpoint, so there is nothing to undo */
  CHECK(aud_doc_undo_label(&d) == NULL);
  CHECK_EQ_INT(d.tracks[0].count, 1);

  aud_spectral_destroy(s);
  aud_spectral_destroy(wrong_rate);
  aud_doc_free(&d);
}

/* Every cleaned-up WAV the suite wrote, so it can be run twice. */
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
    if (strncmp(entry->d_name, AUD_REPAIR_PREFIX, strlen(AUD_REPAIR_PREFIX)) == 0)
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

  RUN(reading_a_range_finds_what_is_in_it);
  RUN(applying_takes_the_hum_out_of_the_track);
  RUN(the_repaired_block_knows_where_it_was_written);
  RUN(a_repair_is_one_press_of_undo);
  RUN(only_the_selected_range_is_touched);
  RUN(a_stereo_take_keeps_both_sides);
  RUN(a_repair_that_cannot_be_done_changes_nothing);

  rc = TEST_RESULT();

  sweep();
  rmdir(TEST_DIR);

  return rc;
}
