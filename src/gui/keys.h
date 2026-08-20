/* SPDX-License-Identifier: MIT */
/*
 * keys.h - the keyboard, and what it is asking for.
 *
 * The window used to read raylib and act on it in the same breath, which made
 * the shortcuts the one part of the app that could only be exercised by sitting
 * in front of it. They are also the part most worth checking: the rules are all
 * about what is allowed to act *now* - a dialog that has the keyboard, a
 * modifier that turns paste into a style change, a space bar that plays or
 * records depending on what the engine is doing - and every one of those is a
 * judgement that reads as obvious and is easy to get quietly wrong.
 *
 * So it is two steps instead of one. app_input_read() is the only thing here
 * that knows raylib exists: it takes a copy of what the keyboard is doing this
 * frame. app_cmd_map() turns that, plus the state of the app, into the list of
 * commands the frame should carry out - and it calls nothing, draws nothing and
 * changes nothing, so a test can hand it a keyboard and read back what the
 * window would have done. actions.c is what carries them out.
 *
 * The order of the list is the order of the old if-chain, because with two keys
 * down in one frame that order is the behaviour.
 */
#ifndef AUDIAKI_GUI_KEYS_H
#define AUDIAKI_GUI_KEYS_H

#include "gui/app.h"

/*
 * Every key the window answers to, and nothing else. A key not in here cannot
 * be bound without being named here first, which is the point: the list of what
 * the app listens to is a list rather than something spread over 300 lines.
 *
 * Aliases are collapsed - the two Deletes, the two pluses - because the app has
 * never wanted to tell them apart and a mapping that could is one more thing to
 * keep two copies of in agreement.
 */
typedef enum
{
  APP_KEY_A = 0,
  APP_KEY_B,
  APP_KEY_C,
  APP_KEY_D,
  APP_KEY_E,
  APP_KEY_F,
  APP_KEY_G,
  APP_KEY_H,
  APP_KEY_I,
  APP_KEY_K,
  APP_KEY_L,
  APP_KEY_M,
  APP_KEY_N,
  APP_KEY_O,
  APP_KEY_R,
  APP_KEY_S,
  APP_KEY_T,
  APP_KEY_V,
  APP_KEY_X,
  APP_KEY_Y,
  APP_KEY_Z,

  APP_KEY_SPACE,
  APP_KEY_ESCAPE,
  APP_KEY_F1,
  APP_KEY_F11,
  APP_KEY_SLASH,

  APP_KEY_LEFT,
  APP_KEY_RIGHT,
  APP_KEY_UP,
  APP_KEY_DOWN,
  APP_KEY_HOME,
  APP_KEY_END,

  APP_KEY_DELETE, /* and backspace, which means the same thing here */
  APP_KEY_COMMA,
  APP_KEY_PERIOD,
  APP_KEY_LEFT_BRACKET,
  APP_KEY_RIGHT_BRACKET,
  APP_KEY_PLUS,  /* and the keypad's */
  APP_KEY_MINUS, /* and the keypad's */

  /* one digit a visualiser style, so the last one is APP_KEY_ONE + COUNT - 1 */
  APP_KEY_ONE,
  APP_KEY_COUNT = APP_KEY_ONE + AUD_VIZ_MODE_COUNT
} app_key;

/*
 * One frame of the keyboard.
 *
 * `pressed` is the key going down and `repeated` is the keyboard's own repeat
 * under a key that is being held. They are separate because most of the window
 * wants the first alone - a held R should not start a take a frame - and the
 * few that walk somewhere (the arrows, the nudges) want both.
 */
typedef struct
{
  int ctrl;
  int shift;
  int alt;
  unsigned char pressed[APP_KEY_COUNT];
  unsigned char repeated[APP_KEY_COUNT];
} app_input;

/* Modifiers carried on a command, for the few whose meaning turns on one. */
#define APP_MOD_SHIFT 0x1
#define APP_MOD_CTRL 0x2
#define APP_MOD_ALT 0x4

/*
 * What a frame of the keyboard came to. The comment on each says what `arg`
 * and `mod` mean for it; the ones with neither use both as zero.
 */
typedef enum
{
  APP_CMD_NONE = 0,

  APP_CMD_HELP_TOGGLE,
  APP_CMD_HELP_CLOSE,
  APP_CMD_MENU_CLOSE,

  APP_CMD_EDIT, /* arg: an app_edit_action */

  /* arg: -1 back, +1 on. mod: SHIFT extends the selection, CTRL steps to the
   * next clip edge, ALT ignores the grid for this one press. */
  APP_CMD_CURSOR_MOVE,
  APP_CMD_CURSOR_HOME, /* mod: SHIFT extends */
  APP_CMD_CURSOR_END,  /* mod: SHIFT extends */
  /* arg: -1 up the stack, +1 down it. mod: SHIFT adds rather than replaces. */
  APP_CMD_STEP_TRACK,
  /* arg: -1 back, +1 on. mod: ALT ignores the grid. */
  APP_CMD_MOVE_SELECTION,

  APP_CMD_TOGGLE_PLAY,
  APP_CMD_TOGGLE_RECORD,
  APP_CMD_STOP, /* whichever of the render, the take and the playback is running */
  APP_CMD_TOGGLE_MONITOR,
  APP_CMD_TOGGLE_LOOP,
  APP_CMD_TOGGLE_CLICK,
  APP_CMD_TOGGLE_GRID,
  APP_CMD_CYCLE_GRID,
  APP_CMD_NUDGE_TEMPO, /* arg: beats, signed */

  APP_CMD_DRAWER_VIZ,
  APP_CMD_DRAWER_SPECTRUM,
  APP_CMD_CYCLE_STYLE,
  APP_CMD_SET_STYLE, /* arg: an aud_viz_mode */
  APP_CMD_FIT,
  APP_CMD_FULLSCREEN,

  APP_CMD_OPEN_DIALOG,
  APP_CMD_EXPORT_DIALOG, /* mod: SHIFT for one WAV a track rather than a mix */
  APP_CMD_PROJECT_SAVE,
  APP_CMD_PROJECT_SAVE_AS,
  APP_CMD_PROJECT_OPEN,

  /*
   * Drop a marker where the cursor is, or take away the one that is already
   * there - one key for both, because a marker is either there or it is not
   * and two keys for that would be one too many.
   */
  APP_CMD_MARK,

  /* arg: +1 the next selected lane, -1 the one before. See app_comp(). */
  APP_CMD_COMP,

  APP_CMD_COUNT
} app_cmd_kind;

typedef struct
{
  app_cmd_kind kind;
  int64_t arg;
  int mod;
} app_cmd;

/*
 * Room for every command one frame can possibly ask for, which is well under
 * this: a frame is bounded by the number of distinct bindings, not by anything
 * a person can type. Sized so app_cmd_map() can never have to drop one.
 */
#define APP_CMD_MAX 40

/* -- keys.c ---------------------------------------------------------------- */

/* Take a copy of what the keyboard is doing. The one thing here that is raylib. */
void app_input_read(app_input *in);

/*
 * This frame's keyboard as commands, in the order they must be carried out.
 * Returns how many were written. Reads `a` and changes nothing.
 */
int app_cmd_map(const app *a, const app_input *in, const aud_engine_status *st,
                app_cmd *out, int max);

/*
 * Where an arrow puts the cursor: the whole of the arrow keys' arithmetic, and
 * the part of it worth checking.
 *
 * `back` is left, `edge` is ctrl - the next clip edge or marker rather than a
 * nudge -
 * `off_grid` is the alt that steps between the beats, and `extend` is the shift
 * that drags the selection along instead of moving the cursor.
 */
uint64_t app_cursor_target(const app *a, int back, int edge, int off_grid, int extend);

/*
 * How far a nudge key moves the selection: to the next grid line when there is
 * a grid, and a fixed number of pixels' worth of time when there is not.
 */
int64_t app_move_step(const app *a, int back, int off_grid);

/* -- actions.c ------------------------------------------------------------- */

/* Carry one out. */
void app_cmd_run(app *a, const app_cmd *cmd, const aud_engine_status *st);

#endif /* AUDIAKI_GUI_KEYS_H */
