/* SPDX-License-Identifier: MIT */
/*
 * What the window's keyboard means.
 *
 * The shortcuts used to be the one part of the app that could only be checked
 * by sitting in front of it: raylib was read and acted on in the same breath,
 * so there was no seam to put a test through. keys.c is that seam - it turns a
 * frame of the keyboard into a list of commands and does nothing else - and
 * these are the rules it holds:
 *
 *   who has the keyboard, when a dialog or an overlay is up
 *   what a modifier turns a key into, Ctrl+V being paste and V a style
 *   what the space bar does, which depends on whether a take is open
 *   where an arrow puts the cursor, which is most of the arithmetic here
 *
 * Every one of those is silent when it is wrong: a key does nothing, or does
 * the wrong thing to a take, and nobody finds out from a crash.
 */
#include "test_util.h"

#include "gui/keys.h"

#include <stdlib.h>

#define TEST_RATE 48000u

/* A window with a project of `seconds` on one lane, and nothing else open. */
static app *window(double seconds)
{
  app *a = calloc(1, sizeof(*a));

  if (a == NULL)
  {
    return NULL;
  }

  aud_doc_init(&a->doc, TEST_RATE);
  /*
   * The one field of the timeline the keyboard reads, set here rather than by
   * aud_timeline_init() so this links against keys.c alone: everything else in
   * timeline.c draws, and a test of the shortcuts should not have to pull the
   * waveform in behind it.
   */
  a->timeline.zoom = AUD_TIMELINE_ZOOM_DEFAULT;
  a->record_track = -1;
  a->last_take_track = -1;

  if (seconds > 0.0)
  {
    size_t frames = (size_t)(seconds * (double)TEST_RATE);
    aud_samples *s = aud_samples_create(1, frames);
    aud_track *t = aud_doc_add_track(&a->doc, "lane", 1);

    if (s != NULL && t != NULL)
    {
      aud_samples_index(s);
      aud_track_add(t, s, 0);
      t->selected = 1;
    }
    aud_samples_release(s);
  }

  return a;
}

static void discard(app *a)
{
  aud_doc_free(&a->doc);
  free(a);
}

/* An idle engine, which is what most of these are asking about. */
static aud_engine_status idle(void)
{
  aud_engine_status st;

  memset(&st, 0, sizeof(st));
  st.state = AUD_ENGINE_IDLE;
  return st;
}

static aud_engine_status recording(void)
{
  aud_engine_status st = idle();

  st.state = AUD_ENGINE_RECORDING;
  return st;
}

/* One key going down, with whichever modifiers are being held with it. */
static app_input press(app_key k, int ctrl, int shift, int alt)
{
  app_input in;

  memset(&in, 0, sizeof(in));
  in.ctrl = ctrl;
  in.shift = shift;
  in.alt = alt;
  in.pressed[k] = 1;
  return in;
}

/* The same key, arriving as the keyboard's own repeat under a held one. */
static app_input hold(app_key k)
{
  app_input in;

  memset(&in, 0, sizeof(in));
  in.repeated[k] = 1;
  return in;
}

/*
 * Map one frame and return how many commands came out, leaving them in `cmds`.
 * Every test goes through here, so none of them has to size an array.
 */
static app_cmd g_cmds[APP_CMD_MAX];

static int map(const app *a, const app_input *in, const aud_engine_status *st)
{
  memset(g_cmds, 0, sizeof(g_cmds));
  return app_cmd_map(a, in, st, g_cmds, APP_CMD_MAX);
}

/* Whether the frame came to exactly one command, of this kind. */
static int only(int count, app_cmd_kind kind)
{
  return count == 1 && g_cmds[0].kind == kind;
}

/* -- who has the keyboard -------------------------------------------------- */

TEST(the_save_dialog_takes_the_keyboard_outright)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->save.open = 1;

  /*
   * R is the one that mattered enough to write down: the dialog is asking
   * where the last take goes, and a window where R started another one while
   * the question was up would lose the take being named.
   */
  {
    app_input in = press(APP_KEY_R, 0, 0, 0);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }
  {
    app_input in = press(APP_KEY_Z, 1, 0, 0);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }
  {
    app_input in = press(APP_KEY_SPACE, 0, 0, 0);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }

  discard(a);
}

TEST(a_question_waiting_stops_everything_behind_it)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in = press(APP_KEY_DELETE, 0, 0, 0);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->confirm.open = 1;

  /* the edit keys least of all: they are what most of the questions are about */
  CHECK_EQ_INT(map(a, &in, &st), 0);

  /* and escape is the dialog's own, not ours - it answers the question "no" */
  {
    app_input esc = press(APP_KEY_ESCAPE, 0, 0, 0);

    CHECK_EQ_INT(map(a, &esc, &st), 0);
  }

  discard(a);
}

TEST(the_device_menu_answers_escape_and_nothing_else)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  a->device_menu_open = 1;

  {
    app_input in = press(APP_KEY_ESCAPE, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_MENU_CLOSE));
  }
  {
    app_input in = press(APP_KEY_R, 0, 0, 0);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }

  discard(a);
}

TEST(three_keys_reach_the_shortcut_list_and_escape_leaves_it)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  const app_key opens[] = {APP_KEY_F1, APP_KEY_SLASH, APP_KEY_H};

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++)
  {
    app_input in = press(opens[i], 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_HELP_TOGGLE));
  }

  a->help_open = 1;

  /* the list sits over the window, so nothing behind it answers */
  {
    app_input in = press(APP_KEY_R, 0, 0, 0);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }
  {
    app_input in = press(APP_KEY_ESCAPE, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_HELP_CLOSE));
  }
  /* ...and the keys that opened it still shut it */
  {
    app_input in = press(APP_KEY_F1, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_HELP_TOGGLE));
  }

  discard(a);
}

/* -- what a modifier turns a key into -------------------------------------- */

TEST(ctrl_tells_the_edits_apart_from_the_letters_they_share)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* V is the collision that made this worth writing down */
  {
    app_input in = press(APP_KEY_V, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_PASTE);
  }
  {
    app_input in = press(APP_KEY_V, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_CYCLE_STYLE));
  }

  /* and C, which is copy behind ctrl and the metronome without it */
  {
    app_input in = press(APP_KEY_C, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_COPY);
  }
  {
    app_input in = press(APP_KEY_C, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_TOGGLE_CLICK));
  }

  discard(a);
}

TEST(shift_turns_an_undo_round_and_a_save_into_a_save_as)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  {
    app_input in = press(APP_KEY_Z, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_UNDO);
  }
  {
    app_input in = press(APP_KEY_Z, 1, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_REDO);
  }
  {
    app_input in = press(APP_KEY_S, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_PROJECT_SAVE));
  }
  {
    app_input in = press(APP_KEY_S, 1, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_PROJECT_SAVE_AS));
  }

  /* the export asks the same question either way, about a different set of
   * files - so it is one command carrying the modifier rather than two */
  {
    app_input in = press(APP_KEY_E, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EXPORT_DIALOG));
    CHECK_EQ_INT(g_cmds[0].mod & APP_MOD_SHIFT, 0);
  }
  {
    app_input in = press(APP_KEY_E, 1, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EXPORT_DIALOG));
    CHECK(g_cmds[0].mod & APP_MOD_SHIFT);
  }

  discard(a);
}

/* -- the transport --------------------------------------------------------- */

TEST(space_plays_unless_there_is_a_take_open)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in = press(APP_KEY_SPACE, 0, 0, 0);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  CHECK(only(map(a, &in, &st), APP_CMD_TOGGLE_PLAY));

  /*
   * Mid-take it stops the take instead. Space is the key nearest the hand and
   * a take is the thing that must not be left running by a key that looked
   * like it did something else.
   */
  {
    aud_engine_status taking = recording();

    CHECK(only(map(a, &in, &taking), APP_CMD_TOGGLE_RECORD));
  }

  /* ctrl+space is the transport's, whatever the engine is doing */
  {
    app_input ctrl_space = press(APP_KEY_SPACE, 1, 0, 0);

    CHECK(only(map(a, &ctrl_space, &st), APP_CMD_TOGGLE_RECORD));
  }

  discard(a);
}

TEST(a_held_key_repeats_only_where_walking_makes_sense)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* leaning on R must not start a take a frame */
  {
    app_input in = hold(APP_KEY_R);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }
  {
    app_input in = hold(APP_KEY_SPACE);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }

  /* the arrows and the nudges are what a key is held down for */
  {
    app_input in = hold(APP_KEY_LEFT);

    CHECK(only(map(a, &in, &st), APP_CMD_CURSOR_MOVE));
  }
  {
    app_input in = hold(APP_KEY_DOWN);

    CHECK(only(map(a, &in, &st), APP_CMD_STEP_TRACK));
  }
  {
    app_input in = hold(APP_KEY_COMMA);

    CHECK(only(map(a, &in, &st), APP_CMD_MOVE_SELECTION));
  }

  discard(a);
}

/* -- where the arrows point ------------------------------------------------ */

TEST(a_bare_arrow_over_a_selection_lands_on_the_end_it_is_heading_for)
{
  app *a = window(8.0);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_select(&a->doc, 2u * TEST_RATE, 5u * TEST_RATE);

  /*
   * As it does over selected text: the eye is looking at the near side of the
   * range, and setting off from the far side of it reads as the cursor jumping
   * somewhere nobody pointed.
   */
  CHECK_EQ_INT(app_cursor_target(a, 1, 0, 0, 0), 2u * TEST_RATE);
  CHECK_EQ_INT(app_cursor_target(a, 0, 0, 0, 0), 5u * TEST_RATE);

  discard(a);
}

TEST(shift_walks_the_far_end_of_the_selection_rather_than_the_cursor)
{
  app *a = window(8.0);
  uint64_t to;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* the cursor sits on the anchor, so the end that moves is the other one */
  aud_doc_select_from(&a->doc, 2u * TEST_RATE, 5u * TEST_RATE);
  CHECK_EQ_INT(a->doc.cursor, 2u * TEST_RATE);

  to = app_cursor_target(a, 0, 0, 0, 1);
  CHECK(to > 5u * TEST_RATE);

  /* and a nudge back off the same end shortens it rather than jumping */
  to = app_cursor_target(a, 1, 0, 0, 1);
  CHECK(to < 5u * TEST_RATE);
  CHECK(to > 2u * TEST_RATE);

  discard(a);
}

/*
 * The gain shares its keys with the tempo, the way paste shares V with the
 * visualiser, and the same modifier tells them apart.
 */
TEST(the_gain_keys_are_the_tempo_keys_behind_ctrl)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  {
    app_input in = press(APP_KEY_PLUS, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_LOUDER);
  }
  {
    app_input in = press(APP_KEY_MINUS, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_QUIETER);
  }
  {
    app_input in = press(APP_KEY_PLUS, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_NUDGE_TEMPO));
  }

  /* and the measured version, where shift is which measurement rather than
   * how big a step - there is only one size of "put it on the target" */
  {
    app_input in = press(APP_KEY_N, 1, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_NORMALIZE_PEAK);
  }
  {
    app_input in = press(APP_KEY_N, 1, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
    CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_NORMALIZE_LOUDNESS);
  }
  {
    app_input in = press(APP_KEY_N, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_DRAWER_SPECTRUM));
  }

  discard(a);
}

TEST(ctrl_steps_to_the_next_clip_edge)
{
  app *a = window(0.0);
  aud_samples *s;
  aud_track *t;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* one lane, one second of audio starting at four seconds in */
  s = aud_samples_create(1, TEST_RATE);
  t = aud_doc_add_track(&a->doc, "lane", 1);
  CHECK(s != NULL && t != NULL);
  if (s == NULL || t == NULL)
  {
    discard(a);
    return;
  }
  aud_samples_index(s);
  aud_track_add(t, s, 4u * TEST_RATE);
  aud_samples_release(s);
  t->selected = 1;

  aud_doc_set_cursor(&a->doc, 0);
  CHECK_EQ_INT(app_cursor_target(a, 0, 1, 0, 0), 4u * TEST_RATE);

  aud_doc_set_cursor(&a->doc, 6u * TEST_RATE);
  CHECK_EQ_INT(app_cursor_target(a, 1, 1, 0, 0), 5u * TEST_RATE);

  /* nothing that way leaves the cursor where it is rather than at zero */
  aud_doc_set_cursor(&a->doc, 0);
  CHECK_EQ_INT(app_cursor_target(a, 1, 1, 0, 0), 0);

  discard(a);
}

TEST(an_arrow_lands_on_the_grid_and_alt_steps_off_it)
{
  app *a = window(30.0);
  uint64_t beat;
  uint64_t on;
  uint64_t off;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /* 120 to the bar of four, divided to the beat: half a second a line */
  aud_doc_set_tempo(&a->doc, 120.0, 4u);
  aud_doc_set_grid(&a->doc, AUD_DOC_GRID_BEAT);
  a->timeline.grid = 1;
  beat = (uint64_t)aud_doc_grid_frames(&a->doc);
  CHECK_EQ_INT(beat, TEST_RATE / 2u);

  /*
   * Landing on the line rather than stepping by its length is the whole point:
   * a take that came in late goes onto the beat, not a beat further on.
   */
  aud_doc_set_cursor(&a->doc, beat + 100u);
  on = app_cursor_target(a, 0, 0, 0, 0);
  CHECK_EQ_INT(on, 2u * beat);

  /* alt is the one edit that has to go between two beats */
  off = app_cursor_target(a, 0, 0, 1, 0);
  CHECK(off != on);
  CHECK(off > beat + 100u);

  /* and with the grid switched off, alt changes nothing */
  a->timeline.grid = 0;
  CHECK_EQ_INT(app_cursor_target(a, 0, 0, 0, 0), off);

  discard(a);
}

TEST(a_nudge_moves_the_selection_to_the_grid_line_it_is_heading_for)
{
  app *a = window(30.0);
  uint64_t beat;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  aud_doc_set_tempo(&a->doc, 120.0, 4u);
  aud_doc_set_grid(&a->doc, AUD_DOC_GRID_BEAT);
  a->timeline.grid = 1;
  beat = (uint64_t)aud_doc_grid_frames(&a->doc);

  /* a selection starting a hair after the first beat moves onto the second */
  aud_doc_select(&a->doc, beat + 100u, beat + 100u + TEST_RATE);
  CHECK_EQ_INT(app_move_step(a, 0, 0), (int64_t)(2u * beat) - (int64_t)(beat + 100u));

  /* ...and back onto the first, which is a shorter distance than a whole beat */
  CHECK_EQ_INT(app_move_step(a, 1, 0), -(int64_t)100);

  /* off the grid it is the same fixed distance either way */
  CHECK_EQ_INT(app_move_step(a, 1, 1), -app_move_step(a, 0, 1));

  discard(a);
}

TEST(home_and_end_reach_both_ends_of_the_project)
{
  app *a = window(7.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  {
    app_input in = press(APP_KEY_HOME, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_CURSOR_HOME));
    CHECK_EQ_INT(g_cmds[0].mod & APP_MOD_SHIFT, 0);
  }
  {
    app_input in = press(APP_KEY_END, 0, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_CURSOR_END));
    CHECK(g_cmds[0].mod & APP_MOD_SHIFT);
  }

  /* and they are presses, not walks: leaning on Home is still one jump */
  {
    app_input in = hold(APP_KEY_HOME);

    CHECK_EQ_INT(map(a, &in, &st), 0);
  }

  discard(a);
}

TEST(up_and_down_walk_the_track_selection_and_shift_adds_to_it)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  {
    app_input in = press(APP_KEY_DOWN, 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_STEP_TRACK));
    CHECK_EQ_INT(g_cmds[0].arg, 1);
    CHECK_EQ_INT(g_cmds[0].mod & APP_MOD_SHIFT, 0);
  }
  {
    app_input in = press(APP_KEY_UP, 0, 1, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_STEP_TRACK));
    CHECK_EQ_INT(g_cmds[0].arg, -1);
    CHECK(g_cmds[0].mod & APP_MOD_SHIFT);
  }

  discard(a);
}

TEST(the_arrows_carry_the_modifiers_the_target_is_worked_out_from)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in = press(APP_KEY_LEFT, 1, 1, 1);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  CHECK(only(map(a, &in, &st), APP_CMD_CURSOR_MOVE));
  CHECK_EQ_INT(g_cmds[0].arg, -1);
  CHECK(g_cmds[0].mod & APP_MOD_SHIFT);
  CHECK(g_cmds[0].mod & APP_MOD_CTRL);
  CHECK(g_cmds[0].mod & APP_MOD_ALT);

  /* right is the other way, and without ctrl it is a nudge rather than an edge */
  {
    app_input right = press(APP_KEY_RIGHT, 0, 0, 0);

    CHECK(only(map(a, &right, &st), APP_CMD_CURSOR_MOVE));
    CHECK_EQ_INT(g_cmds[0].arg, 1);
    CHECK_EQ_INT(g_cmds[0].mod, 0);
  }

  discard(a);
}

/* -- more than one at a time ----------------------------------------------- */

TEST(two_keys_in_one_frame_come_out_in_the_order_they_are_carried_out)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  memset(&in, 0, sizeof(in));
  in.ctrl = 1;
  in.pressed[APP_KEY_X] = 1;
  in.pressed[APP_KEY_V] = 1;

  /*
   * Cut before paste, because that is the order the old if-chain ran them in
   * and with two edit keys down in one frame the order is the behaviour.
   */
  CHECK_EQ_INT(map(a, &in, &st), 2);
  CHECK_EQ_INT(g_cmds[0].kind, APP_CMD_EDIT);
  CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_CUT);
  CHECK_EQ_INT(g_cmds[1].kind, APP_CMD_EDIT);
  CHECK_EQ_INT(g_cmds[1].arg, APP_EDIT_PASTE);

  discard(a);
}

TEST(ctrl_holds_back_the_bare_keys_behind_it)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /*
   * Ctrl+F is not a fit and ctrl+B is not the visualiser panel. Nothing is
   * bound to either, and a window that fell through to the bare meaning would
   * rearrange itself under a chord aimed at something else entirely.
   */
  memset(&in, 0, sizeof(in));
  in.ctrl = 1;
  in.pressed[APP_KEY_B] = 1;
  in.pressed[APP_KEY_F] = 1;
  CHECK_EQ_INT(map(a, &in, &st), 0);

  /* and the two that ctrl does give a meaning of their own are not the bare
   * ones: ctrl+M marks the ruler where M alone is the monitor, and ctrl+L
   * limits where L alone is the loop */
  in = press(APP_KEY_M, 1, 0, 0);
  CHECK(only(map(a, &in, &st), APP_CMD_MARK));

  in = press(APP_KEY_L, 1, 0, 0);
  CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
  CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_LIMIT);

  discard(a);
}

/*
 * K walks the comp, and alt+K silences instead. Both are bare keys, so the
 * thing worth checking is that neither is reachable with ctrl held - where K
 * already means split.
 */
TEST(k_walks_the_comp_and_ctrl_still_splits)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  in = press(APP_KEY_K, 0, 0, 0);
  CHECK(only(map(a, &in, &st), APP_CMD_COMP));
  CHECK_EQ_INT(g_cmds[0].arg, 1);

  in = press(APP_KEY_K, 0, 1, 0);
  CHECK(only(map(a, &in, &st), APP_CMD_COMP));
  CHECK_EQ_INT(g_cmds[0].arg, -1);

  in = press(APP_KEY_K, 0, 0, 1);
  CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
  CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_MUTE_TOGGLE);

  in = press(APP_KEY_K, 1, 0, 0);
  CHECK(only(map(a, &in, &st), APP_CMD_EDIT));
  CHECK_EQ_INT(g_cmds[0].arg, APP_EDIT_SPLIT);

  discard(a);
}

/*
 * Ctrl+arrow lands on markers as well as on clip edges. One key for "the next
 * thing worth landing on" rather than two that each know about half of them.
 */
TEST(ctrl_arrow_steps_to_a_marker_as_well_as_to_a_clip_edge)
{
  app *a = window(4.0);

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  CHECK(aud_doc_mark(&a->doc, TEST_RATE, "chorus") >= 0);
  aud_doc_set_cursor(&a->doc, 0);

  /* the lane runs 0 to 4 s in one piece, so its only edge ahead is the end -
   * and the marker at one second is nearer */
  CHECK_EQ_INT(app_cursor_target(a, 0, 1, 0, 0), TEST_RATE);

  aud_doc_set_cursor(&a->doc, 2u * TEST_RATE);
  CHECK_EQ_INT(app_cursor_target(a, 1, 1, 0, 0), TEST_RATE);

  discard(a);
}

TEST(every_style_has_a_digit_of_its_own)
{
  app *a = window(4.0);
  aud_engine_status st = idle();

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    app_input in = press((app_key)(APP_KEY_ONE + i), 0, 0, 0);

    CHECK(only(map(a, &in, &st), APP_CMD_SET_STYLE));
    CHECK_EQ_INT(g_cmds[0].arg, i);
  }

  discard(a);
}

TEST(a_frame_of_every_key_at_once_still_fits)
{
  app *a = window(4.0);
  aud_engine_status st = idle();
  app_input in;
  int count;

  CHECK(a != NULL);
  if (a == NULL)
  {
    return;
  }

  /*
   * Not a gesture anybody makes - but APP_CMD_MAX is a promise that mapping
   * never has to drop a command on the floor, and this is what checks it is
   * still big enough as bindings are added.
   */
  memset(&in, 0, sizeof(in));
  for (int k = 0; k < APP_KEY_COUNT; k++)
  {
    in.pressed[k] = 1;
    in.repeated[k] = 1;
  }

  /* F1 is in there, and the shortcut list takes the frame the moment it is */
  in.pressed[APP_KEY_F1] = 0;
  in.pressed[APP_KEY_SLASH] = 0;
  in.pressed[APP_KEY_H] = 0;
  in.pressed[APP_KEY_ESCAPE] = 0;

  count = map(a, &in, &st);
  CHECK(count > 0);
  CHECK(count < APP_CMD_MAX);

  discard(a);
}

int main(void)
{
  RUN(the_save_dialog_takes_the_keyboard_outright);
  RUN(a_question_waiting_stops_everything_behind_it);
  RUN(the_device_menu_answers_escape_and_nothing_else);
  RUN(three_keys_reach_the_shortcut_list_and_escape_leaves_it);
  RUN(ctrl_tells_the_edits_apart_from_the_letters_they_share);
  RUN(shift_turns_an_undo_round_and_a_save_into_a_save_as);
  RUN(space_plays_unless_there_is_a_take_open);
  RUN(a_held_key_repeats_only_where_walking_makes_sense);
  RUN(a_bare_arrow_over_a_selection_lands_on_the_end_it_is_heading_for);
  RUN(shift_walks_the_far_end_of_the_selection_rather_than_the_cursor);
  RUN(the_gain_keys_are_the_tempo_keys_behind_ctrl);
  RUN(ctrl_steps_to_the_next_clip_edge);
  RUN(an_arrow_lands_on_the_grid_and_alt_steps_off_it);
  RUN(a_nudge_moves_the_selection_to_the_grid_line_it_is_heading_for);
  RUN(home_and_end_reach_both_ends_of_the_project);
  RUN(up_and_down_walk_the_track_selection_and_shift_adds_to_it);
  RUN(the_arrows_carry_the_modifiers_the_target_is_worked_out_from);
  RUN(two_keys_in_one_frame_come_out_in_the_order_they_are_carried_out);
  RUN(ctrl_holds_back_the_bare_keys_behind_it);
  RUN(k_walks_the_comp_and_ctrl_still_splits);
  RUN(ctrl_arrow_steps_to_a_marker_as_well_as_to_a_clip_edge);
  RUN(every_style_has_a_digit_of_its_own);
  RUN(a_frame_of_every_key_at_once_still_fits);
  return TEST_RESULT();
}
