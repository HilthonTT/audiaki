/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/project.h"

#include "edit/edit.h"
#include "edit/load.h"
#include "media/wav.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR "audiaki-project-test"
#define TEST_RATE 8000u

/* A WAV of `frames` frames whose every sample says which frame it came from. */
static void write_wav(const char *path, unsigned channels, size_t frames)
{
  wav_writer w;
  int16_t *pcm = malloc(frames * channels * sizeof(*pcm));

  for (size_t f = 0; f < frames; f++)
  {
    for (unsigned c = 0; c < channels; c++)
    {
      pcm[f * channels + c] = (int16_t)(f + c * 1000u);
    }
  }

  CHECK_EQ_INT(wav_open(&w, path, TEST_RATE, (uint16_t)channels, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, frames * channels * sizeof(*pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);
  free(pcm);
}

static void in_dir(char *dst, size_t size, const char *name)
{
  snprintf(dst, size, "%s/%s", TEST_DIR, name);
}

/* A project of two tracks over two takes, with an edit and a fade on it. */
static void build(aud_doc *d)
{
  char one[256];
  char two[256];

  in_dir(one, sizeof(one), "take-001.wav");
  in_dir(two, sizeof(two), "take-002.wav");

  aud_doc_init(d, 0);
  CHECK_EQ_INT(aud_edit_load_wav(d, one, NULL), 0);
  CHECK_EQ_INT(aud_edit_load_wav(d, two, NULL), 1);

  /* a split, so the second track is two clips over one block */
  CHECK_EQ_INT(aud_track_split(&d->tracks[1], 400), 0);
  CHECK_EQ_INT(d->tracks[1].count, 2);

  d->tracks[0].gain = 0.75f;
  d->tracks[0].pan = -0.5f;
  d->tracks[1].muted = 1;
  d->tracks[1].height = 200;
  snprintf(d->tracks[0].name, sizeof(d->tracks[0].name), "Guitar DI");

  CHECK_EQ_INT(aud_track_fade_in_at(&d->tracks[1], 400, 64), 0);
  aud_doc_select(d, 100, 300);
}

/* The first channel at `frame`; the buffer is a whole frame, however wide. */
static float at(const aud_track *t, uint64_t frame)
{
  float one[64];

  aud_track_read(t, frame, one, 1);
  return one[0];
}

TEST(a_project_comes_back_as_it_went_in)
{
  aud_doc saved;
  aud_doc opened;
  char path[256];
  const char *why = NULL;

  in_dir(path, sizeof(path), "session" AUD_PROJECT_EXT);
  build(&saved);

  CHECK_EQ_INT(aud_project_save(&saved, path, &why), 0);
  CHECK(why == NULL);

  aud_doc_init(&opened, 0);
  CHECK_EQ_INT(aud_project_load(&opened, path, &why), 0);

  CHECK_EQ_INT(opened.rate, TEST_RATE);
  CHECK_EQ_INT(opened.count, saved.count);
  CHECK_EQ_INT((int)opened.sel_start, 100);
  CHECK_EQ_INT((int)opened.sel_end, 300);

  CHECK(strcmp(opened.tracks[0].name, "Guitar DI") == 0);
  CHECK_EQ_DBL(opened.tracks[0].gain, 0.75, 1e-6);
  CHECK_EQ_DBL(opened.tracks[0].pan, -0.5, 1e-6);
  CHECK_EQ_INT(opened.tracks[1].muted, 1);
  CHECK_EQ_INT(opened.tracks[1].height, 200);
  CHECK_EQ_INT(opened.tracks[1].channels, 2);

  /* the clip layout, including the split and the fade on the second half */
  CHECK_EQ_INT(opened.tracks[1].count, 2);
  CHECK_EQ_INT((int)opened.tracks[1].clips[1].start, 400);
  CHECK_EQ_INT((int)opened.tracks[1].clips[1].fade_in, 64);

  /* and the audio itself reads back the same, fade and all */
  for (uint64_t f = 0; f < 900; f += 137)
  {
    CHECK(at(&opened.tracks[0], f) == at(&saved.tracks[0], f));
    CHECK(at(&opened.tracks[1], f) == at(&saved.tracks[1], f));
  }

  aud_doc_free(&opened);
  aud_doc_free(&saved);
}

TEST(the_tempo_is_saved_with_the_session)
{
  aud_doc saved;
  aud_doc opened;
  char path[256];
  const char *why = NULL;

  in_dir(path, sizeof(path), "tempo" AUD_PROJECT_EXT);
  build(&saved);
  aud_doc_set_tempo(&saved, 137.5, 7u);

  CHECK_EQ_INT(aud_project_save(&saved, path, &why), 0);

  aud_doc_init(&opened, 0);
  CHECK_EQ_INT(aud_project_load(&opened, path, &why), 0);
  CHECK_EQ_DBL(opened.tempo, 137.5, 1e-6);
  CHECK_EQ_INT(opened.beats_per_bar, 7);

  aud_doc_free(&opened);
  aud_doc_free(&saved);
}

/*
 * The reason the tempo is a line rather than a version bump: a session written
 * before there was one has to open, and open counting on something.
 */
TEST(a_session_with_no_tempo_line_opens_at_the_default)
{
  aud_doc d;
  char path[256];
  const char *why = NULL;
  FILE *f;

  in_dir(path, sizeof(path), "untimed" AUD_PROJECT_EXT);
  f = fopen(path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  fprintf(f, "%s %d\nrate %u\n", AUD_PROJECT_MAGIC, AUD_PROJECT_VERSION, TEST_RATE);
  fclose(f);

  aud_doc_init(&d, 0);
  CHECK_EQ_INT(aud_project_load(&d, path, &why), 0);
  CHECK_EQ_DBL(d.tempo, AUD_DOC_DEFAULT_TEMPO, 1e-9);
  CHECK_EQ_INT(d.beats_per_bar, AUD_CLICK_DEFAULT_BEATS);

  aud_doc_free(&d);
}

/*
 * A file somebody has edited by hand: the beats may be missing, and the number
 * may be one no metronome would play. Neither is a reason to refuse a session
 * whose audio is perfectly good.
 */
TEST(a_hand_written_tempo_is_taken_as_far_as_it_can_be)
{
  aud_doc d;
  char path[256];
  const char *why = NULL;
  FILE *f;

  in_dir(path, sizeof(path), "byhand" AUD_PROJECT_EXT);
  f = fopen(path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  fprintf(f, "%s %d\nrate %u\ntempo 90\n", AUD_PROJECT_MAGIC, AUD_PROJECT_VERSION,
          TEST_RATE);
  fclose(f);

  aud_doc_init(&d, 0);
  CHECK_EQ_INT(aud_project_load(&d, path, &why), 0);
  CHECK_EQ_DBL(d.tempo, 90.0, 1e-9);
  CHECK_EQ_INT(d.beats_per_bar, AUD_CLICK_DEFAULT_BEATS);
  aud_doc_free(&d);

  f = fopen(path, "wb");
  CHECK(f != NULL);
  if (f == NULL)
  {
    return;
  }
  fprintf(f, "%s %d\nrate %u\ntempo 100000 4\n", AUD_PROJECT_MAGIC, AUD_PROJECT_VERSION,
          TEST_RATE);
  fclose(f);

  aud_doc_init(&d, 0);
  CHECK_EQ_INT(aud_project_load(&d, path, &why), 0);
  CHECK_EQ_DBL(d.tempo, AUD_CLICK_BPM_MAX, 1e-9);
  aud_doc_free(&d);
}

TEST(two_clips_over_one_take_share_the_block_again)
{
  aud_doc saved;
  aud_doc opened;
  char path[256];

  in_dir(path, sizeof(path), "shared" AUD_PROJECT_EXT);
  build(&saved);

  CHECK_EQ_INT(aud_project_save(&saved, path, NULL), 0);
  aud_doc_init(&opened, 0);
  CHECK_EQ_INT(aud_project_load(&opened, path, NULL), 0);

  /*
   * The whole point of the source table: a split take is one file, read once,
   * with two clips over it - not the same audio held twice.
   */
  CHECK(opened.tracks[1].clips[0].audio == opened.tracks[1].clips[1].audio);
  CHECK_EQ_INT(opened.tracks[1].clips[0].audio->refs, 2);

  aud_doc_free(&opened);
  aud_doc_free(&saved);
}

TEST(sources_beside_the_project_are_stored_relative)
{
  aud_doc d;
  char path[256];
  char line[512];
  FILE *f;
  int saw_relative = 0;

  in_dir(path, sizeof(path), "relative" AUD_PROJECT_EXT);
  build(&d);
  CHECK_EQ_INT(aud_project_save(&d, path, NULL), 0);

  f = fopen(path, "rb");
  CHECK(f != NULL);
  while (fgets(line, sizeof(line), f) != NULL)
  {
    if (strncmp(line, "source ", 7) == 0)
    {
      /* beside the project, so it is named by itself and carries no folder */
      CHECK(strcmp(line + 7, "take-001.wav\n") == 0 ||
            strcmp(line + 7, "take-002.wav\n") == 0);
      saw_relative++;
    }
  }
  fclose(f);
  CHECK_EQ_INT(saw_relative, 2);

  aud_doc_free(&d);
}

TEST(a_missing_take_is_named_and_nothing_is_replaced)
{
  aud_doc d;
  aud_doc open_already;
  char path[256];
  char gone[256];
  char keep[256];
  const char *why = NULL;

  in_dir(path, sizeof(path), "missing" AUD_PROJECT_EXT);
  in_dir(gone, sizeof(gone), "take-002.wav");
  in_dir(keep, sizeof(keep), "take-002.keep");

  build(&d);
  CHECK_EQ_INT(aud_project_save(&d, path, NULL), 0);
  aud_doc_free(&d);

  CHECK_EQ_INT(rename(gone, keep), 0);

  /* a project already open, which a failed load must not touch */
  aud_doc_init(&open_already, 0);
  {
    char one[256];

    in_dir(one, sizeof(one), "take-001.wav");
    CHECK_EQ_INT(aud_edit_load_wav(&open_already, one, NULL), 0);
  }

  CHECK_EQ_INT(aud_project_load(&open_already, path, &why), -1);
  CHECK(why != NULL);
  CHECK(strstr(why, "take-002.wav") != NULL);

  /* what was open is still open */
  CHECK_EQ_INT(open_already.count, 1);
  CHECK_EQ_INT((int)aud_track_end(&open_already.tracks[0]), 500);

  CHECK_EQ_INT(rename(keep, gone), 0);
  aud_doc_free(&open_already);
}

TEST(rubbish_is_refused_rather_than_half_read)
{
  aud_doc d;
  char path[256];
  const char *why = NULL;
  FILE *f;

  in_dir(path, sizeof(path), "rubbish" AUD_PROJECT_EXT);
  f = fopen(path, "wb");
  CHECK(f != NULL);
  fputs("this is not a project\nrate 48000\n", f);
  fclose(f);

  aud_doc_init(&d, 44100);
  CHECK_EQ_INT(aud_project_load(&d, path, &why), -1);
  CHECK(why != NULL);
  CHECK_EQ_INT(d.count, 0);
  CHECK_EQ_INT(d.rate, 44100); /* untouched */

  /* and a file that is not there at all */
  CHECK_EQ_INT(aud_project_load(&d, TEST_DIR "/nothing" AUD_PROJECT_EXT, &why), -1);
  CHECK(why != NULL);

  aud_doc_free(&d);
}

TEST(an_unsaved_block_is_named_rather_than_dropped)
{
  aud_doc d;
  aud_samples *loose = aud_samples_create(1, 100);
  char path[256];
  const char *why = NULL;
  aud_track *t;

  in_dir(path, sizeof(path), "loose" AUD_PROJECT_EXT);

  aud_doc_init(&d, TEST_RATE);
  t = aud_doc_add_track(&d, "Scratch", 1);
  CHECK_EQ_INT(aud_track_add(t, loose, 0), 0);
  aud_samples_release(loose);

  CHECK_EQ_INT(aud_project_save(&d, path, &why), -1);
  CHECK(why != NULL);
  CHECK(strstr(why, "Scratch") != NULL);

  aud_doc_free(&d);
}

TEST(the_extension_is_recognised_whatever_case_it_is_in)
{
  CHECK(aud_project_is_project("a" AUD_PROJECT_EXT));
  CHECK(aud_project_is_project("/x/y/session.AKI"));
  CHECK(!aud_project_is_project(AUD_PROJECT_EXT)); /* a hidden file, not a name */
  CHECK(!aud_project_is_project("take.wav"));
  CHECK(!aud_project_is_project(NULL));
}

int main(void)
{
  char one[256];
  char two[256];
  int rc;

  CHECK_EQ_INT(aud_path_mkdirs(TEST_DIR), 0);
  in_dir(one, sizeof(one), "take-001.wav");
  in_dir(two, sizeof(two), "take-002.wav");
  write_wav(one, 1, 500);
  write_wav(two, 2, 900);

  RUN(a_project_comes_back_as_it_went_in);
  RUN(the_tempo_is_saved_with_the_session);
  RUN(a_session_with_no_tempo_line_opens_at_the_default);
  RUN(a_hand_written_tempo_is_taken_as_far_as_it_can_be);
  RUN(two_clips_over_one_take_share_the_block_again);
  RUN(sources_beside_the_project_are_stored_relative);
  RUN(a_missing_take_is_named_and_nothing_is_replaced);
  RUN(rubbish_is_refused_rather_than_half_read);
  RUN(an_unsaved_block_is_named_rather_than_dropped);
  RUN(the_extension_is_recognised_whatever_case_it_is_in);

  rc = TEST_RESULT();

  /* the suite leaves nothing behind, so it can be run again */
  remove(one);
  remove(two);
  {
    static const char *const leftovers[] = {"session", "shared",  "relative",
                                            "missing", "rubbish", "loose",
                                            "tempo",   "untimed", "byhand"};

    for (size_t i = 0; i < sizeof(leftovers) / sizeof(leftovers[0]); i++)
    {
      char path[256];

      snprintf(path, sizeof(path), "%s/%s%s", TEST_DIR, leftovers[i], AUD_PROJECT_EXT);
      remove(path);
    }
  }
  rmdir(TEST_DIR);

  return rc;
}
