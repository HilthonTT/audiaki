/* SPDX-License-Identifier: MIT */
/*
 * The copy of the session that survives the window dying.
 *
 * Worth testing for the reason confirm.c is: every rule here is about what
 * happens when something goes wrong, so getting one of them wrong is silent
 * until the day it matters. A recovery file that is never written, or one that
 * is written over the session itself, or one left behind after a clean exit and
 * offered back as if something had happened - none of those announces itself.
 *
 * It is testable because deciding is kept apart from drawing and from the
 * clock: app_autosave_step() takes the time as an argument and touches no
 * pixels, so a test can hand it an afternoon in a few lines.
 */
#include "test_util.h"

#include "gui/app.h"

#include "media/wav.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#define TEST_DIR "audiaki-autosave-test"
#define TEST_RATE 8000u

/* What autosave.c reaches for and this test has no window to give it. */
void app_set_status(app *a, const char *fmt, ...)
{
  (void)a;
  (void)fmt;
}

static void in_dir(char *dst, size_t size, const char *name)
{
  snprintf(dst, size, "%s/%s", TEST_DIR, name);
}

/* A WAV for the takes to be, so a recovered project has something to open. */
static void write_wav(const char *path, size_t frames)
{
  wav_writer w;
  int16_t *pcm = calloc(frames, sizeof(*pcm));

  for (size_t f = 0; f < frames; f++)
  {
    pcm[f] = (int16_t)f;
  }

  CHECK_EQ_INT(wav_open(&w, path, TEST_RATE, 1, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, frames * sizeof(*pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);
  free(pcm);
}

/*
 * A window with one lane of audio on it, the take named so that a project can
 * be written for it - a block with no source is one no project can refer to,
 * which is the state a take is in while it is still arriving.
 */
static app *window(void)
{
  app *a = calloc(1, sizeof(*a));
  aud_samples *s;
  aud_track *t;
  char take[256];

  if (a == NULL)
  {
    return NULL;
  }

  in_dir(take, sizeof(take), "take-001.wav");
  aud_doc_init(&a->doc, TEST_RATE);
  snprintf(a->take_dir, sizeof(a->take_dir), "%s", TEST_DIR);
  a->record_track = -1;
  a->last_take_track = -1;

  s = aud_samples_create(1, 200);
  t = aud_doc_add_track(&a->doc, "lane", 1);
  if (s != NULL && t != NULL)
  {
    aud_samples_index(s);
    aud_samples_set_source(s, take);
    aud_track_add(t, s, 0);
  }
  aud_samples_release(s);
  return a;
}

static void discard(app *a)
{
  if (a != NULL)
  {
    aud_doc_free(&a->doc);
    free(a);
  }
}

static int there(const char *path)
{
  return access(path, F_OK) == 0;
}

/* Backdate a file, for the questions that are about which is the newer. */
static void aged(const char *path, int seconds)
{
  struct stat st;
  struct utimbuf when;

  CHECK_EQ_INT(stat(path, &st), 0);
  when.actime = st.st_atime;
  when.modtime = st.st_mtime - seconds;
  CHECK_EQ_INT(utime(path, &when), 0);
}

TEST(a_named_session_keeps_its_recovery_beside_itself)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  snprintf(a->project_path, sizeof(a->project_path), "%s/song" AUD_PROJECT_EXT, TEST_DIR);
  CHECK_EQ_INT(app_recovery_path(a, path, sizeof(path)), 0);
  CHECK_EQ_STR(path, TEST_DIR "/song" AUD_PROJECT_EXT APP_RECOVER_EXT);

  /* and one that has never been saved keeps it where the takes are */
  a->project_path[0] = '\0';
  CHECK_EQ_INT(app_recovery_path(a, path, sizeof(path)), 0);
  CHECK_EQ_STR(path, TEST_DIR "/" APP_RECOVER_UNNAMED);

  discard(a);
}

TEST(the_first_edit_is_written_out_rather_than_waited_on)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  remove(path);

  /* nothing has changed, so there is nothing to keep */
  app_autosave_step(a, 1.0);
  CHECK(!there(path));

  /* the first edit is the least protected work there is, and does not wait */
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));
  CHECK_EQ_STR(a->recovery_path, path);

  remove(path);
  discard(a);
}

TEST(it_is_written_at_the_interval_and_not_every_frame)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  remove(path);

  a->project_dirty = 1;
  app_autosave_step(a, 100.0);
  CHECK(there(path));
  remove(path);

  /* a frame later, and half the interval later, it is not due */
  app_autosave_step(a, 100.02);
  app_autosave_step(a, 100.0 + APP_AUTOSAVE_SECONDS / 2.0);
  CHECK(!there(path));

  app_autosave_step(a, 100.0 + APP_AUTOSAVE_SECONDS);
  CHECK(there(path));

  remove(path);
  discard(a);
}

TEST(saving_the_session_takes_the_recovery_file_away)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));

  /* what a save leaves behind: nothing to recover, so nothing kept */
  a->project_dirty = 0;
  app_autosave_step(a, 2.0);
  CHECK(!there(path));
  CHECK_EQ_STR(a->recovery_path, "");

  /* and the next edit after it is written straight away, not in half a minute */
  a->project_dirty = 1;
  app_autosave_step(a, 3.0);
  CHECK(there(path));

  remove(path);
  discard(a);
}

TEST(a_save_as_moves_the_recovery_file_rather_than_leaving_one_behind)
{
  app *a = window();
  char unnamed[AUD_PATH_MAX];
  char named[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, unnamed, sizeof(unnamed));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(unnamed));

  /* saved as something, and then edited again */
  snprintf(a->project_path, sizeof(a->project_path), "%s/song" AUD_PROJECT_EXT, TEST_DIR);
  app_recovery_path(a, named, sizeof(named));
  remove(named);

  app_autosave_step(a, 2.0);
  CHECK(!there(unnamed)); /* the old one is taken away, not abandoned */
  CHECK(there(named));

  remove(named);
  discard(a);
}

TEST(nothing_is_written_while_a_take_is_still_arriving)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  remove(path);

  a->project_dirty = 1;
  a->record_track = 0;
  app_autosave_step(a, 1.0);
  app_autosave_step(a, 1000.0);
  CHECK(!there(path));

  /* and the moment it stops, the wait is already over */
  a->record_track = -1;
  app_autosave_step(a, 1000.02);
  CHECK(there(path));

  remove(path);
  discard(a);
}

TEST(a_write_that_cannot_happen_is_said_once_and_not_retried_every_frame)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* a lane holding audio that is nowhere on disk, which no project can name */
  {
    aud_samples *s = aud_samples_create(1, 10);
    aud_track *t = aud_doc_add_track(&a->doc, "unsaved", 1);

    aud_samples_index(s);
    aud_track_add(t, s, 0);
    aud_samples_release(s);
  }

  app_recovery_path(a, path, sizeof(path));
  remove(path);

  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(!there(path));
  CHECK_EQ_INT(a->recovery_failed, 1);
  CHECK_EQ_DBL(a->recovery_at, 1.0, 1e-9); /* held off, rather than retried */

  discard(a);
}

TEST(a_recovery_file_left_behind_is_opened_at_startup)
{
  app *a = window();
  app *back;
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* a window that died with unsaved edits */
  aud_doc_add_track(&a->doc, "second lane", 1);
  app_recovery_path(a, path, sizeof(path));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));
  discard(a);

  /* and the one that comes up afterwards */
  back = calloc(1, sizeof(*back));
  CHECK(back != NULL);
  if (back == NULL)
  {
    return;
  }
  aud_doc_init(&back->doc, TEST_RATE);
  snprintf(back->take_dir, sizeof(back->take_dir), "%s", TEST_DIR);
  back->record_track = -1;

  app_recover(back);
  CHECK_EQ_INT(back->doc.count, 2);
  CHECK_EQ_STR(back->recovery_path, path);
  /* what is on screen is not what any file holds, and the title says so */
  CHECK_EQ_INT(back->project_dirty, 1);

  remove(path);
  discard(back);
}

TEST(a_window_that_came_up_with_something_in_it_leaves_the_unnamed_one_alone)
{
  app *a = window();
  app *back;
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));
  discard(a);

  /* this one was given a WAV on the command line: the recovery is not its own */
  back = window();
  CHECK(back != NULL);
  if (back == NULL)
  {
    return;
  }

  app_recover(back);
  CHECK_EQ_INT(back->doc.count, 1);
  CHECK_EQ_INT(back->project_dirty, 0);
  CHECK(there(path)); /* still there for a window that comes up empty */

  remove(path);
  discard(back);
}

TEST(a_recovery_older_than_the_session_it_shadows_is_thrown_away)
{
  app *a = window();
  char project[AUD_PATH_MAX];
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  in_dir(project, sizeof(project), "shadowed" AUD_PROJECT_EXT);
  snprintf(a->project_path, sizeof(a->project_path), "%s", project);
  CHECK_EQ_INT(aud_project_save(&a->doc, project, NULL), 0);

  app_recovery_path(a, path, sizeof(path));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));

  /*
   * The session was saved after this was written, so it describes a state that
   * has already been superseded: recovering it would be offering to undo a save.
   */
  aged(path, 120);
  a->recovery_path[0] = '\0';
  app_recover(a);
  CHECK(!there(path));
  CHECK_EQ_INT(a->project_dirty, 1); /* untouched, rather than reloaded */

  remove(project);
  discard(a);
}

TEST(a_shutdown_that_could_not_write_the_edits_keeps_the_recovery)
{
  app *a = window();
  char path[AUD_PATH_MAX];

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  app_recovery_path(a, path, sizeof(path));
  a->project_dirty = 1;
  app_autosave_step(a, 1.0);
  CHECK(there(path));

  /* the way out with the edits written: it has done its job */
  app_autosave_done(a, 0);
  CHECK(there(path)); /* ...and with them not written, it is the only copy */

  app_autosave_done(a, 1);
  CHECK(!there(path));

  discard(a);
}

int main(void)
{
  char take[256];
  int rc;

  CHECK_EQ_INT(aud_path_mkdirs(TEST_DIR), 0);
  in_dir(take, sizeof(take), "take-001.wav");
  write_wav(take, 200);

  RUN(a_named_session_keeps_its_recovery_beside_itself);
  RUN(the_first_edit_is_written_out_rather_than_waited_on);
  RUN(it_is_written_at_the_interval_and_not_every_frame);
  RUN(saving_the_session_takes_the_recovery_file_away);
  RUN(a_save_as_moves_the_recovery_file_rather_than_leaving_one_behind);
  RUN(nothing_is_written_while_a_take_is_still_arriving);
  RUN(a_write_that_cannot_happen_is_said_once_and_not_retried_every_frame);
  RUN(a_recovery_file_left_behind_is_opened_at_startup);
  RUN(a_window_that_came_up_with_something_in_it_leaves_the_unnamed_one_alone);
  RUN(a_recovery_older_than_the_session_it_shadows_is_thrown_away);
  RUN(a_shutdown_that_could_not_write_the_edits_keeps_the_recovery);

  rc = TEST_RESULT();

  /* the suite leaves nothing behind, so it can be run again */
  remove(take);
  {
    static const char *const leftovers[] = {
        APP_RECOVER_UNNAMED, "song" AUD_PROJECT_EXT APP_RECOVER_EXT,
        "shadowed" AUD_PROJECT_EXT, "shadowed" AUD_PROJECT_EXT APP_RECOVER_EXT};

    for (size_t i = 0; i < sizeof(leftovers) / sizeof(leftovers[0]); i++)
    {
      char path[256];

      in_dir(path, sizeof(path), leftovers[i]);
      remove(path);
    }
  }
  rmdir(TEST_DIR);

  return rc;
}
