/* SPDX-License-Identifier: MIT */
/*
 * Which actions the window stops to ask about.
 *
 * The only part of src/gui with tests, and it earned them: the rule here is a
 * judgement about what is worth interrupting somebody for, it is easy to get
 * subtly wrong, and getting it wrong is silent - an edit that should have asked
 * simply does not, and nobody finds out until something is gone.
 *
 * It is testable at all because deciding is kept apart from drawing:
 * app_confirm_edit() reads the document and writes a question into `app`, and
 * touches no pixels. The dialog is what draws it, and that is not tested here.
 */
#include "test_util.h"

#include "gui/app.h"

#include <stdlib.h>

#define TEST_RATE 48000u

/*
 * What confirm.c reaches for when a question is answered. Nothing here answers
 * one, so these are never called; they exist because the linker wants them.
 */
void app_edit_now(app *a, app_edit_action action)
{
  (void)a;
  (void)action;
}

void app_set_status(app *a, const char *fmt, ...)
{
  (void)a;
  (void)fmt;
}

int aud_repair_panel_apply(aud_repair_panel *p, aud_doc *d, const char *dir)
{
  (void)p;
  (void)d;
  (void)dir;
  return 0;
}

/* A project of `tracks` lanes, each holding `seconds` of audio from frame 0. */
static app *project(double seconds, int tracks)
{
  app *a = calloc(1, sizeof(*a));

  if (a == NULL)
  {
    return NULL;
  }

  aud_doc_init(&a->doc, TEST_RATE);

  for (int i = 0; i < tracks; i++)
  {
    size_t frames = (size_t)(seconds * (double)TEST_RATE);
    aud_samples *s = aud_samples_create(1, frames);
    aud_track *t = aud_doc_add_track(&a->doc, "lane", 1);

    if (s == NULL || t == NULL)
    {
      continue;
    }
    aud_samples_index(s);
    aud_track_add(t, s, 0);
    aud_samples_release(s);
  }

  return a;
}

static void discard(app *a)
{
  aud_doc_free(&a->doc);
  free(a);
}

/* Select the whole of one lane without going through select-all. */
static void select_range(app *a, double from, double to)
{
  aud_doc_select(&a->doc, (uint64_t)(from * TEST_RATE), (uint64_t)(to * TEST_RATE));
}

TEST(ctrl_a_then_delete_asks_however_short_the_project_is)
{
  app *a = project(6.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);

  /*
   * Six seconds is under APP_CONFIRM_SECONDS, and for a while that was enough
   * to let this through without a word - which is the one gesture that empties
   * a session outright. Emptying it is the reason to ask, not its length.
   */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 1);
  CHECK(a->confirm.open);
  CHECK(strstr(a->confirm.title, "everything") != NULL);
  CHECK(strstr(a->confirm.title, "1 track") != NULL);

  discard(a);
}

TEST(emptying_several_lanes_counts_all_of_them)
{
  app *a = project(4.0, 3);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);

  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 1);
  CHECK(strstr(a->confirm.title, "3 tracks") != NULL);
  /* three lanes of four seconds is twelve seconds of audio, not four */
  CHECK(strstr(a->confirm.reason[0], "12.0 seconds") != NULL);

  discard(a);
}

TEST(a_range_across_lanes_is_worth_what_all_of_them_hold)
{
  app *a = project(60.0, 6);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_tracks(&a->doc, 1);
  select_range(a, 10.0, 12.0);

  /*
   * Two seconds on the ruler, twelve seconds of audio. Measuring the selection
   * rather than what it covers was the other half of the same bug.
   */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 1);
  CHECK(strstr(a->confirm.title, "12.0 seconds") != NULL);

  discard(a);
}

TEST(a_small_edit_on_one_lane_is_not_interrupted)
{
  app *a = project(60.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->doc.tracks[0].selected = 1;
  select_range(a, 10.0, 12.0);

  /* the whole point of the threshold: routine editing is not a dialog */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 0);
  CHECK(!a->confirm.open);

  discard(a);
}

TEST(an_edit_after_an_undo_asks_whatever_its_size)
{
  app *a = project(60.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->doc.tracks[0].selected = 1;
  select_range(a, 10.0, 11.0);

  aud_doc_checkpoint(&a->doc, "delete");
  CHECK_EQ_INT(aud_doc_undo(&a->doc), 0);
  CHECK_EQ_INT(a->doc.redo_count, 1);

  /*
   * One second, which on its own would go through in silence. What is being
   * asked about is the step that would stop being redoable, which is the loss
   * nothing else in the window would ever mention.
   */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 1);
  CHECK(strstr(a->confirm.title, "lose what you undid") != NULL);
  CHECK(a->confirm.irreversible);

  discard(a);
}

TEST(trimming_to_everything_keeps_everything_and_says_nothing)
{
  app *a = project(6.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);

  /* a trim that covers the whole project throws nothing away */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_TRIM), 0);
  CHECK(!a->confirm.open);

  discard(a);
}

TEST(trimming_to_a_sliver_of_a_long_take_asks)
{
  app *a = project(60.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->doc.tracks[0].selected = 1;
  select_range(a, 30.0, 31.0);

  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_TRIM), 1);
  CHECK(strstr(a->confirm.title, "Throw away") != NULL);
  CHECK(strstr(a->confirm.title, "59.0 seconds") != NULL);

  discard(a);
}

TEST(silencing_everything_empties_it_too)
{
  app *a = project(3.0, 2);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);

  /* the lanes survive, but there is no audio left on any of them */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_SILENCE), 1);
  CHECK(strstr(a->confirm.title, "everything") != NULL);

  discard(a);
}

TEST(the_actions_that_discard_nothing_never_ask)
{
  app *a = project(600.0, 4);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);

  /* ten minutes selected on four lanes, and none of these takes any of it */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_COPY), 0);
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_SELECT_ALL), 0);
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_REDO), 0);
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_SPLIT), 0);
  /* a move discards nothing however far it goes: it stops against what is in
   * the way rather than writing over it */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_MOVE), 0);
  CHECK(!a->confirm.open);

  discard(a);
}

TEST(one_question_at_a_time)
{
  app *a = project(6.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select_all(&a->doc);
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_DELETE), 1);

  /*
   * A second question while one is up would replace it, and the answer meant
   * for the first would land on the second. So everything is refused instead.
   */
  CHECK_EQ_INT(app_confirm_edit(a, APP_EDIT_TRIM), 1);
  CHECK_EQ_INT(app_confirm_undo(a), 1);
  CHECK_EQ_INT(a->confirm.kind, APP_CONFIRM_EDIT);
  CHECK_EQ_INT(a->confirm.action, APP_EDIT_DELETE);

  app_confirm_dismiss(a);
  CHECK(!a->confirm.open);

  discard(a);
}

TEST(closing_a_lane_always_asks)
{
  app *a = project(2.0, 2);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* two seconds, nothing selected, nothing undone - and it still asks */
  CHECK_EQ_INT(app_confirm_close_track(a, 0), 1);
  CHECK(strstr(a->confirm.title, "Close") != NULL);
  CHECK_EQ_INT(a->confirm.kind, APP_CONFIRM_CLOSE_TRACK);
  CHECK_EQ_INT(a->confirm.track, 0);

  app_confirm_dismiss(a);

  /* a lane that is not there is not a question, it is nothing to do */
  CHECK_EQ_INT(app_confirm_close_track(a, 9), 0);

  discard(a);
}

TEST(quitting_asks_only_when_something_would_be_lost)
{
  app *a = project(6.0, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->record_track = -1;
  a->project_dirty = 0;
  CHECK_EQ_INT(app_confirm_quit(a), 0);

  a->project_dirty = 1;
  CHECK_EQ_INT(app_confirm_quit(a), 1);
  CHECK_EQ_INT(a->confirm.kind, APP_CONFIRM_QUIT);

  /* it says where the edits go rather than only that they exist */
  {
    int named = 0;

    for (int i = 0; i < a->confirm.reasons; i++)
    {
      if (strstr(a->confirm.reason[i], "recovered") != NULL)
      {
        named = 1;
      }
    }
    CHECK(named);
  }

  discard(a);
}

int main(void)
{
  RUN(ctrl_a_then_delete_asks_however_short_the_project_is);
  RUN(emptying_several_lanes_counts_all_of_them);
  RUN(a_range_across_lanes_is_worth_what_all_of_them_hold);
  RUN(a_small_edit_on_one_lane_is_not_interrupted);
  RUN(an_edit_after_an_undo_asks_whatever_its_size);
  RUN(trimming_to_everything_keeps_everything_and_says_nothing);
  RUN(trimming_to_a_sliver_of_a_long_take_asks);
  RUN(silencing_everything_empties_it_too);
  RUN(the_actions_that_discard_nothing_never_ask);
  RUN(one_question_at_a_time);
  RUN(closing_a_lane_always_asks);
  RUN(quitting_asks_only_when_something_would_be_lost);
  return TEST_RESULT();
}
