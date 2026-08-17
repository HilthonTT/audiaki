/* SPDX-License-Identifier: MIT */
/*
 * keys.c - reading the keyboard, and working out what it meant.
 *
 * Split down the middle. app_input_read() is the only function in the window
 * that asks raylib what is being pressed; everything below it works off the
 * copy it took, so the rules can be exercised without a keyboard, a window or a
 * device. See keys.h for why that division is where it is.
 *
 * Nothing here changes anything. app_cmd_map() reads the app and writes a list;
 * actions.c is what carries the list out.
 */
#include "gui/keys.h"

#include "raylib.h"

#include <string.h>

/* -- reading it ------------------------------------------------------------ */

/*
 * The one table that says which raylib key each of ours is. A key with two
 * spellings has both, so the app can go on not caring which Delete or which
 * plus was pressed.
 */
static const int key_codes[APP_KEY_COUNT][2] = {
    [APP_KEY_A] = {KEY_A, 0},
    [APP_KEY_B] = {KEY_B, 0},
    [APP_KEY_C] = {KEY_C, 0},
    [APP_KEY_D] = {KEY_D, 0},
    [APP_KEY_E] = {KEY_E, 0},
    [APP_KEY_F] = {KEY_F, 0},
    [APP_KEY_G] = {KEY_G, 0},
    [APP_KEY_H] = {KEY_H, 0},
    [APP_KEY_I] = {KEY_I, 0},
    [APP_KEY_K] = {KEY_K, 0},
    [APP_KEY_L] = {KEY_L, 0},
    [APP_KEY_M] = {KEY_M, 0},
    [APP_KEY_N] = {KEY_N, 0},
    [APP_KEY_O] = {KEY_O, 0},
    [APP_KEY_R] = {KEY_R, 0},
    [APP_KEY_S] = {KEY_S, 0},
    [APP_KEY_T] = {KEY_T, 0},
    [APP_KEY_V] = {KEY_V, 0},
    [APP_KEY_X] = {KEY_X, 0},
    [APP_KEY_Y] = {KEY_Y, 0},
    [APP_KEY_Z] = {KEY_Z, 0},
    [APP_KEY_SPACE] = {KEY_SPACE, 0},
    [APP_KEY_ESCAPE] = {KEY_ESCAPE, 0},
    [APP_KEY_F1] = {KEY_F1, 0},
    [APP_KEY_F11] = {KEY_F11, 0},
    [APP_KEY_SLASH] = {KEY_SLASH, 0},
    [APP_KEY_LEFT] = {KEY_LEFT, 0},
    [APP_KEY_RIGHT] = {KEY_RIGHT, 0},
    [APP_KEY_UP] = {KEY_UP, 0},
    [APP_KEY_DOWN] = {KEY_DOWN, 0},
    [APP_KEY_HOME] = {KEY_HOME, 0},
    [APP_KEY_END] = {KEY_END, 0},
    [APP_KEY_DELETE] = {KEY_DELETE, KEY_BACKSPACE},
    [APP_KEY_COMMA] = {KEY_COMMA, 0},
    [APP_KEY_PERIOD] = {KEY_PERIOD, 0},
    [APP_KEY_LEFT_BRACKET] = {KEY_LEFT_BRACKET, 0},
    [APP_KEY_RIGHT_BRACKET] = {KEY_RIGHT_BRACKET, 0},
    [APP_KEY_PLUS] = {KEY_EQUAL, KEY_KP_ADD},
    [APP_KEY_MINUS] = {KEY_MINUS, KEY_KP_SUBTRACT},
};

void app_input_read(app_input *in)
{
  memset(in, 0, sizeof(*in));

  in->ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
  in->shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
  in->alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);

  for (int k = 0; k < APP_KEY_COUNT; k++)
  {
    /* the digits are consecutive in both enumerations, so they need no table */
    int code = k >= APP_KEY_ONE ? KEY_ONE + (k - APP_KEY_ONE) : key_codes[k][0];
    int alias = k >= APP_KEY_ONE ? 0 : key_codes[k][1];

    if (code == 0)
    {
      continue;
    }

    in->pressed[k] =
        (unsigned char)(IsKeyPressed(code) || (alias != 0 && IsKeyPressed(alias)));
    in->repeated[k] = (unsigned char)(IsKeyPressedRepeat(code) ||
                                      (alias != 0 && IsKeyPressedRepeat(alias)));
  }
}

/* Pressed this frame, ignoring the keyboard's repeat under a held key. */
static int hit(const app_input *in, app_key k)
{
  return in->pressed[k] != 0;
}

/* ...or repeating under one, for the keys that walk somewhere. */
static int walk(const app_input *in, app_key k)
{
  return in->pressed[k] != 0 || in->repeated[k] != 0;
}

/* -- where the arrows point ------------------------------------------------ */

/*
 * How far one press of an arrow moves the cursor: the time a few pixels covers
 * at the current zoom.
 *
 * Not a fixed number of seconds, because there is no one right answer to that.
 * Zoomed out to a whole session an arrow should cover ground; zoomed in on a
 * transient it should land on a sample. A distance in pixels is the same
 * gesture at both, and is the one the eye is judging anyway.
 */
#define APP_NUDGE_PIXELS 8.0

static uint64_t app_nudge(const app *a)
{
  double frames;

  if (a->doc.rate == 0 || !(a->timeline.zoom > 0.0))
  {
    return 1;
  }

  frames = APP_NUDGE_PIXELS / a->timeline.zoom * (double)a->doc.rate;
  return frames >= 1.0 ? (uint64_t)(frames + 0.5) : 1u;
}

/* Frames from `at`, without running off the front of the timeline. */
static uint64_t app_step(uint64_t at, uint64_t by, int back)
{
  if (!back)
  {
    return at + by;
  }
  return at > by ? at - by : 0;
}

/*
 * The end of the selection an arrow is moving. The cursor sits on the anchor -
 * see aud_doc_select_from() - so the other end is whichever one it is not, and
 * with no range the two are the same place.
 */
static uint64_t app_moving_edge(const aud_doc *d)
{
  if (d->sel_end <= d->sel_start)
  {
    return d->cursor;
  }
  return d->cursor == d->sel_start ? d->sel_end : d->sel_start;
}

/* The nearest clip edge either way, across the selected tracks - or all of
 * them when none is selected, so the keys work before anything is picked. */
static uint64_t app_clip_edge(const app *a, uint64_t from, int back)
{
  int only_selected = aud_doc_any_track_selected(&a->doc);
  uint64_t best = from;

  for (size_t i = 0; i < a->doc.count; i++)
  {
    const aud_track *t = &a->doc.tracks[i];
    uint64_t edge;

    if (only_selected && !t->selected)
    {
      continue;
    }

    edge = back ? aud_track_edge_before(t, from) : aud_track_edge_after(t, from);
    if (edge == from)
    {
      continue; /* nothing that way on this lane */
    }
    if (best == from || (back ? edge > best : edge < best))
    {
      best = edge;
    }
  }
  return best;
}

/* Whether the grid is on and has something to snap to, alt not being held. */
static int on_grid(const app *a, int off_grid)
{
  return a->timeline.grid && !off_grid && aud_doc_grid_frames(&a->doc) > 0.0;
}

uint64_t app_cursor_target(const app *a, int back, int edge, int off_grid, int extend)
{
  uint64_t from;

  /*
   * A bare arrow over a selection puts it down at the end it is heading for,
   * as it does over selected text, rather than setting off from the far side
   * of a range the eye is looking at the near side of.
   */
  if (!extend && aud_doc_has_range(&a->doc))
  {
    return back ? a->doc.sel_start : a->doc.sel_end;
  }

  from = extend ? app_moving_edge(&a->doc) : a->doc.cursor;

  if (edge)
  {
    return app_clip_edge(a, from, back);
  }

  /*
   * On the grid, an arrow steps from one line to the next rather than by a
   * fixed number of pixels - which is the only way a keyboard can put an edit
   * where the pointer would, and the same alt that drops the pointer off the
   * grid drops the keys off it too.
   */
  if (on_grid(a, off_grid))
  {
    return aud_doc_grid_step(&a->doc, from, back);
  }

  return app_step(from, app_nudge(a), back);
}

/*
 * The same distance the arrow keys walk the cursor by, and for the same reason:
 * on the grid it is a grid line, so a take lands where the bars are, and off it
 * a fixed number of pixels, which is the same gesture at every zoom. Landing on
 * the line rather than stepping by its length is what puts a take that came in
 * late onto the beat rather than moving it a beat further on.
 */
int64_t app_move_step(const app *a, int back, int off_grid)
{
  const aud_doc *d = &a->doc;
  uint64_t by;

  if (on_grid(a, off_grid))
  {
    uint64_t to = aud_doc_grid_step(d, d->sel_start, back);

    return to >= d->sel_start ? (int64_t)(to - d->sel_start)
                              : -(int64_t)(d->sel_start - to);
  }

  by = app_nudge(a);
  return back ? -(int64_t)by : (int64_t)by;
}

/* -- the mapping ----------------------------------------------------------- */

/*
 * The list being written, and the one place a command is added to it.
 *
 * The bound cannot be reached - APP_CMD_MAX is above the number of distinct
 * bindings, so every key in the window going down at once still fits - but it
 * is checked rather than assumed, because the alternative to checking is
 * writing past the caller's array the day somebody adds the fortieth.
 */
typedef struct
{
  app_cmd *out;
  int max;
  int count;
} cmd_list;

static void emit(cmd_list *l, app_cmd_kind kind, int64_t arg, int mod)
{
  if (l->count >= l->max)
  {
    return;
  }

  l->out[l->count].kind = kind;
  l->out[l->count].arg = arg;
  l->out[l->count].mod = mod;
  l->count++;
}

static void emit_edit(cmd_list *l, app_edit_action action)
{
  emit(l, APP_CMD_EDIT, (int64_t)action, 0);
}

/*
 * Everything the arrow keys do. Left and right are time - a nudge, a clip edge
 * with ctrl, the whole project with home and end - and up and down are which
 * lanes are selected. Shift extends the selection instead of moving the cursor,
 * as it does in every editor with a keyboard.
 */
static void map_arrows(const app_input *in, cmd_list *l, int ctrl)
{
  int left = walk(in, APP_KEY_LEFT);
  int right = walk(in, APP_KEY_RIGHT);
  int mod = 0;

  mod |= in->shift ? APP_MOD_SHIFT : 0;
  mod |= ctrl ? APP_MOD_CTRL : 0;
  mod |= in->alt ? APP_MOD_ALT : 0;

  if (left || right)
  {
    emit(l, APP_CMD_CURSOR_MOVE, left ? -1 : 1, mod);
  }

  if (hit(in, APP_KEY_HOME))
  {
    emit(l, APP_CMD_CURSOR_HOME, 0, mod & APP_MOD_SHIFT);
  }
  if (hit(in, APP_KEY_END))
  {
    emit(l, APP_CMD_CURSOR_END, 0, mod & APP_MOD_SHIFT);
  }

  if (walk(in, APP_KEY_DOWN))
  {
    emit(l, APP_CMD_STEP_TRACK, 1, mod & APP_MOD_SHIFT);
  }
  if (walk(in, APP_KEY_UP))
  {
    emit(l, APP_CMD_STEP_TRACK, -1, mod & APP_MOD_SHIFT);
  }
}

/*
 * The edits, behind Ctrl, so the single letters the window has always answered
 * to keep meaning what they meant. Ctrl+V is paste and V is the next visualiser
 * style, which is only a collision if the modifier is not looked at.
 */
static void map_ctrl(const app_input *in, cmd_list *l)
{
  int shift = in->shift ? APP_MOD_SHIFT : 0;

  if (hit(in, APP_KEY_Z))
  {
    emit_edit(l, shift ? APP_EDIT_REDO : APP_EDIT_UNDO);
  }
  if (hit(in, APP_KEY_Y))
  {
    emit_edit(l, APP_EDIT_REDO);
  }
  if (hit(in, APP_KEY_X))
  {
    emit_edit(l, APP_EDIT_CUT);
  }
  if (hit(in, APP_KEY_C))
  {
    emit_edit(l, APP_EDIT_COPY);
  }
  if (hit(in, APP_KEY_V))
  {
    emit_edit(l, APP_EDIT_PASTE);
  }
  if (hit(in, APP_KEY_A))
  {
    emit_edit(l, APP_EDIT_SELECT_ALL);
  }
  if (hit(in, APP_KEY_D))
  {
    emit_edit(l, APP_EDIT_DUPLICATE);
  }
  if (hit(in, APP_KEY_T))
  {
    emit_edit(l, APP_EDIT_TRIM);
  }
  if (hit(in, APP_KEY_K))
  {
    emit_edit(l, APP_EDIT_SPLIT);
  }

  /* E mixes it down, shift+E writes one WAV a track instead */
  if (hit(in, APP_KEY_E))
  {
    emit(l, APP_CMD_EXPORT_DIALOG, 0, shift);
  }

  /* the session itself: S writes it, shift+S asks where, O opens one */
  if (hit(in, APP_KEY_S))
  {
    emit(l, shift ? APP_CMD_PROJECT_SAVE_AS : APP_CMD_PROJECT_SAVE, 0, 0);
  }
  if (hit(in, APP_KEY_O))
  {
    emit(l, APP_CMD_PROJECT_OPEN, 0, 0);
  }

  /* ctrl+arrow steps between clip edges rather than by a nudge */
  map_arrows(in, l, 1);

  /* the transport, where the editor's own space bar has displaced it */
  if (hit(in, APP_KEY_SPACE))
  {
    emit(l, APP_CMD_TOGGLE_RECORD, 0, 0);
  }
}

static void map_plain(const app_input *in, const aud_engine_status *st, cmd_list *l)
{
  int shift = in->shift ? APP_MOD_SHIFT : 0;
  int alt = in->alt ? APP_MOD_ALT : 0;

  map_arrows(in, l, 0);

  /* the fades, on the bracket keys the selection edges look like */
  if (hit(in, APP_KEY_LEFT_BRACKET))
  {
    emit_edit(l, APP_EDIT_FADE_IN);
  }
  if (hit(in, APP_KEY_RIGHT_BRACKET))
  {
    emit_edit(l, APP_EDIT_FADE_OUT);
  }

  /* and the nudge, on the two keys that already point either way */
  if (walk(in, APP_KEY_COMMA))
  {
    emit(l, APP_CMD_MOVE_SELECTION, -1, alt);
  }
  if (walk(in, APP_KEY_PERIOD))
  {
    emit(l, APP_CMD_MOVE_SELECTION, 1, alt);
  }

  if (hit(in, APP_KEY_DELETE))
  {
    emit_edit(l, APP_EDIT_DELETE);
  }

  /*
   * Space plays, the way it does in every editor, and ctrl+space records. The
   * window used to record on space, when there was nothing to play; now that
   * there is, the commoner of the two gets the bare key.
   */
  if (hit(in, APP_KEY_SPACE))
  {
    int taking = st->state == AUD_ENGINE_RECORDING || st->state == AUD_ENGINE_PAUSED;

    emit(l, taking ? APP_CMD_TOGGLE_RECORD : APP_CMD_TOGGLE_PLAY, 0, 0);
  }

  if (hit(in, APP_KEY_R))
  {
    emit(l, APP_CMD_TOGGLE_RECORD, 0, 0);
  }
  if (hit(in, APP_KEY_I))
  {
    emit(l, APP_CMD_OPEN_DIALOG, 0, 0);
  }
  if (hit(in, APP_KEY_B))
  {
    emit(l, APP_CMD_DRAWER_VIZ, 0, 0);
  }
  if (hit(in, APP_KEY_N))
  {
    emit(l, APP_CMD_DRAWER_SPECTRUM, 0, 0);
  }

  /* the three that count time: the loop, the metronome and the grid it beats on */
  if (hit(in, APP_KEY_L))
  {
    emit(l, APP_CMD_TOGGLE_LOOP, 0, 0);
  }
  if (hit(in, APP_KEY_C))
  {
    emit(l, APP_CMD_TOGGLE_CLICK, 0, 0);
  }
  if (hit(in, APP_KEY_G))
  {
    /*
     * Shift+G walks what the grid is divided into rather than turning it off,
     * so the two things a grid has - whether it is on, and how fine it is - are
     * one key apart instead of one being buried in a menu.
     */
    emit(l, shift ? APP_CMD_CYCLE_GRID : APP_CMD_TOGGLE_GRID, 0, 0);
  }

  /* the tempo, without having to reach for the spinner */
  if (hit(in, APP_KEY_PLUS))
  {
    emit(l, APP_CMD_NUDGE_TEMPO, shift ? 10 : 1, 0);
  }
  if (hit(in, APP_KEY_MINUS))
  {
    emit(l, APP_CMD_NUDGE_TEMPO, shift ? -10 : -1, 0);
  }

  if (hit(in, APP_KEY_S))
  {
    emit(l, APP_CMD_STOP, 0, 0);
  }
  if (hit(in, APP_KEY_M))
  {
    emit(l, APP_CMD_TOGGLE_MONITOR, 0, 0);
  }
  if (hit(in, APP_KEY_V))
  {
    emit(l, APP_CMD_CYCLE_STYLE, 0, 0);
  }

  /*
   * F fits the project to the window, which is what it does in every editor of
   * this kind and what anyone looking at a waveform reaches for. Fullscreen has
   * moved to F11, where a window manager would have put it anyway.
   */
  if (hit(in, APP_KEY_F))
  {
    emit(l, APP_CMD_FIT, 0, 0);
  }
  if (hit(in, APP_KEY_F11))
  {
    emit(l, APP_CMD_FULLSCREEN, 0, 0);
  }

  /* straight to a style, for the one you keep coming back to */
  for (int i = 0; i < AUD_VIZ_MODE_COUNT; i++)
  {
    if (hit(in, (app_key)(APP_KEY_ONE + i)))
    {
      emit(l, APP_CMD_SET_STYLE, i, 0);
    }
  }
}

int app_cmd_map(const app *a, const app_input *in, const aud_engine_status *st,
                app_cmd *out, int max)
{
  cmd_list l = {out, max, 0};

  /*
   * The dialog is asking where the last take goes and has fields to type into,
   * so it takes the keyboard outright - and takes it first, because a window
   * where R starts a new take while the last one is being named would lose the
   * one being named.
   */
  if (a->save.open)
  {
    return 0;
  }

  /*
   * A question is waiting on an answer, so nothing behind it may act - least
   * of all the edit keys, which are what most of the questions are about.
   * Escape answers it "no", and the dialog itself reads that.
   */
  if (a->confirm.open)
  {
    return 0;
  }

  /* the menu owns the keyboard while it is open, same as it owns the mouse */
  if (a->device_menu_open)
  {
    if (hit(in, APP_KEY_ESCAPE))
    {
      emit(&l, APP_CMD_MENU_CLOSE, 0, 0);
    }
    return l.count;
  }

  if (hit(in, APP_KEY_F1) || hit(in, APP_KEY_SLASH) || hit(in, APP_KEY_H))
  {
    emit(&l, APP_CMD_HELP_TOGGLE, 0, 0);
    return l.count;
  }

  /* the shortcut list sits over the window, so nothing behind it answers */
  if (a->help_open)
  {
    if (hit(in, APP_KEY_ESCAPE))
    {
      emit(&l, APP_CMD_HELP_CLOSE, 0, 0);
    }
    return l.count;
  }

  if (in->ctrl)
  {
    map_ctrl(in, &l);
    return l.count;
  }

  map_plain(in, st, &l);
  return l.count;
}
