/* SPDX-License-Identifier: MIT */
/*
 * confirm.c - the question that stops something until it is answered.
 *
 * Everything the editor does is undoable, which is exactly why this file has
 * to be careful about what it asks. A confirmation in front of every edit is a
 * click bought with nothing, and one that appears constantly is one nobody
 * reads - at which point the dangerous case is worse off than it was, because
 * the dialog has been trained into a reflex.
 *
 * So there are two grounds for asking, and an action needs one of them:
 *
 *   it is large     more than APP_CONFIRM_SECONDS of audio, or a whole lane.
 *                   Undoable, but not something to discover afterwards.
 *   it loses        something no undo will bring back: the redo future, a file
 *                   nothing will point at any more, a session never saved.
 *
 * The second is the one worth having, and it is the one that is invisible
 * without it. Undo is the safety net and is not itself guarded - guarding the
 * way back out of a mistake would be the wrong thing to make harder.
 *
 * The dialog decides nothing. It holds a question and what was about to
 * happen; app_confirm_draw() hands the action back to whoever can carry it
 * out. That is what keeps it from having to know what any of these mean.
 */
#include "gui/app.h"

#include "gui/ui.h"

#include "edit/project.h"
#include "edit/repair.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CONFIRM_W 520.0f
#define CONFIRM_PAD 24.0f
#define CONFIRM_ROW 22.0f
#define CONFIRM_BUTTON_W 132.0f
#define CONFIRM_BUTTON_H 30.0f

static void ask(app *a, app_confirm_kind kind, const char *accept, const char *fmt, ...)
    AUD_PRINTF(4, 5);

static void ask(app *a, app_confirm_kind kind, const char *accept, const char *fmt, ...)
{
  va_list args;

  memset(&a->confirm, 0, sizeof(a->confirm));
  a->confirm.open = 1;
  a->confirm.kind = kind;
  snprintf(a->confirm.accept, sizeof(a->confirm.accept), "%s", accept);

  va_start(args, fmt);
  vsnprintf(a->confirm.title, sizeof(a->confirm.title), fmt, args);
  va_end(args);
}

/* One line of "and here is why". Silently ignored past APP_CONFIRM_REASONS. */
static void because(app *a, int irreversible, const char *fmt, ...) AUD_PRINTF(3, 4);

static void because(app *a, int irreversible, const char *fmt, ...)
{
  va_list args;

  if (a->confirm.reasons >= APP_CONFIRM_REASONS)
  {
    return;
  }

  va_start(args, fmt);
  vsnprintf(a->confirm.reason[a->confirm.reasons], APP_CONFIRM_LINE, fmt, args);
  va_end(args);
  a->confirm.reasons++;

  if (irreversible)
  {
    a->confirm.irreversible = 1;
  }
}

void app_confirm_dismiss(app *a)
{
  memset(&a->confirm, 0, sizeof(a->confirm));
}

/* -- what is worth asking about --------------------------------------------- */

/*
 * Whether `action` takes a checkpoint, and so would throw away the redo stack.
 * The ones that do not are the ones that change no audio: undo and redo
 * themselves, copying, and selecting.
 */
static int takes_checkpoint(app_edit_action action)
{
  switch (action)
  {
  case APP_EDIT_UNDO:
  case APP_EDIT_REDO:
  case APP_EDIT_COPY:
  case APP_EDIT_SELECT_ALL:
    return 0;
  default:
    return 1;
  }
}

/*
 * Seconds of audio `action` would remove or overwrite, added up across every
 * lane it touches. Zero for the actions that discard nothing.
 *
 * Added up, rather than taken from the selection's length, because the length
 * on its own is the wrong number: a range across five selected lanes takes
 * five times what it looks like on the ruler.
 */
static double edit_seconds(const app *a, app_edit_action action)
{
  const aud_doc *d = &a->doc;
  double rate = (double)d->rate;
  uint64_t total = 0;

  if (!(rate > 0.0))
  {
    return 0.0;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];
    uint64_t end;
    uint64_t lo;
    uint64_t hi;

    if (!t->selected)
    {
      continue;
    }

    /* the selection, held to what this lane actually reaches */
    end = aud_track_end(t);
    lo = d->sel_start < end ? d->sel_start : end;
    hi = d->sel_end < end ? d->sel_end : end;

    switch (action)
    {
    case APP_EDIT_TRIM:
      total += end - (hi - lo); /* what it throws away is everything else */
      break;
    case APP_EDIT_CUT:
    case APP_EDIT_DELETE:
    case APP_EDIT_SILENCE:
      total += hi - lo;
      break;
    default:
      /*
       * Paste, split, duplicate, the fades and the gain move edges about or
       * change a number on a clip; none of them discards audio.
       */
      break;
    }
  }

  return (double)total / rate;
}

/* Selected lanes with any audio on them, for a question that counts them. */
static size_t lanes_touched(const app *a)
{
  size_t lanes = 0;

  for (size_t i = 0; i < a->doc.count; i++)
  {
    if (a->doc.tracks[i].selected && aud_track_end(&a->doc.tracks[i]) > 0)
    {
      lanes++;
    }
  }
  return lanes;
}

/*
 * Whether the action would leave no audio anywhere in the project.
 *
 * Asked about whatever its length, because emptying a session is a different
 * act from taking a bit out of one and a short session is not a cheap one.
 * This is the Ctrl+A, Delete gesture, which a threshold in seconds lets
 * straight through on anything under it.
 */
static int clears_everything(const app *a, app_edit_action action)
{
  const aud_doc *d = &a->doc;
  int any = 0;

  if (action != APP_EDIT_DELETE && action != APP_EDIT_CUT && action != APP_EDIT_SILENCE &&
      action != APP_EDIT_TRIM)
  {
    return 0;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];
    uint64_t end = aud_track_end(t);

    if (end == 0)
    {
      continue; /* an empty lane has nothing to lose either way */
    }
    any = 1;

    /* a lane left out, or left partly covered, is a lane something survives on */
    if (!t->selected || d->sel_start > 0 || d->sel_end < end)
    {
      return 0;
    }
  }

  /*
   * Trimming to a selection that covers everything keeps everything, so it is
   * the one action here that clears nothing when the whole project is picked.
   */
  return any && action != APP_EDIT_TRIM;
}

/* How an action reads in a sentence, for the question and the button on it. */
static const char *edit_name(app_edit_action action)
{
  switch (action)
  {
  case APP_EDIT_CUT:
    return "Cut";
  case APP_EDIT_DELETE:
    return "Delete";
  case APP_EDIT_SILENCE:
    return "Silence";
  case APP_EDIT_TRIM:
    return "Trim";
  case APP_EDIT_PASTE:
    return "Paste";
  case APP_EDIT_SPLIT:
    return "Split";
  case APP_EDIT_DUPLICATE:
    return "Copy to";
  case APP_EDIT_FADE_IN:
    return "Fade in";
  case APP_EDIT_FADE_OUT:
    return "Fade out";
  case APP_EDIT_MOVE:
    return "Move";
  case APP_EDIT_LOUDER:
    return "Turn up";
  case APP_EDIT_QUIETER:
    return "Turn down";
  case APP_EDIT_NORMALIZE_PEAK:
  case APP_EDIT_NORMALIZE_LOUDNESS:
    return "Normalize";
  default:
    return "That";
  }
}

/* "the 7 steps you could have redone", said only when there are any. */
static void warn_about_redo(app *a)
{
  size_t steps = a->doc.redo_count;

  if (steps == 0)
  {
    return;
  }

  because(a, 1, "%zu step%s you could have redone will go, and no undo brings %s back.",
          steps, steps == 1 ? "" : "s", steps == 1 ? "it" : "them");
}

int app_confirm_edit(app *a, app_edit_action action)
{
  double seconds;
  int everything;
  int big;

  if (a->confirm.open)
  {
    return 1; /* something is already being asked; nothing else may start */
  }

  if (!takes_checkpoint(action))
  {
    return 0;
  }

  seconds = edit_seconds(a, action);
  everything = clears_everything(a, action);
  big = everything || seconds > APP_CONFIRM_SECONDS;

  if (!big && a->doc.redo_count == 0)
  {
    return 0;
  }

  ask(a, APP_CONFIRM_EDIT, edit_name(action), "%s %.1f seconds of audio?",
      action == APP_EDIT_TRIM ? "Throw away" : edit_name(action), seconds);
  a->confirm.action = action;

  if (everything)
  {
    /*
     * The length is beside the point here - what matters is that there is
     * nothing left afterwards, and that is what the question says.
     */
    size_t lanes = lanes_touched(a);

    snprintf(a->confirm.title, sizeof(a->confirm.title), "%s everything on %zu track%s?",
             edit_name(action), lanes, lanes == 1 ? "" : "s");
    because(a, 0, "%.1f seconds of audio, and the project is left empty.", seconds);
    because(a, 0, "Ctrl+Z puts it back.");
  }
  else if (!big)
  {
    /*
     * The edit itself is unremarkable and the redo stack is the whole reason
     * for stopping, so the question says that rather than quoting a length
     * that was never the point.
     */
    snprintf(a->confirm.title, sizeof(a->confirm.title), "%s, and lose what you undid?",
             edit_name(action));
  }
  else if (action == APP_EDIT_TRIM)
  {
    because(a, 0, "Everything outside the selection goes. Ctrl+Z puts it back.");
  }
  else
  {
    because(a, 0, "Ctrl+Z puts it back.");
  }

  warn_about_redo(a);
  return 1;
}

int app_confirm_close_track(app *a, size_t index)
{
  const aud_track *t;
  double seconds;

  if (a->confirm.open)
  {
    return 1;
  }
  if (index >= a->doc.count)
  {
    return 0;
  }

  t = &a->doc.tracks[index];
  seconds = a->doc.rate > 0 ? (double)aud_track_end(t) / (double)a->doc.rate : 0.0;

  /*
   * Always asked, whatever is on the lane. Every other edit works on a range
   * you can see highlighted; this one is a small button next to a name, and
   * what it takes is everything on that lane - which is exactly the shape of
   * click somebody makes by accident.
   */
  ask(a, APP_CONFIRM_CLOSE_TRACK, "Close it", "Close %.40s?", t->name);
  a->confirm.track = (long)index;
  because(a, 0, "%.1f seconds of audio on it goes with it. Ctrl+Z puts it back.",
          seconds);
  warn_about_redo(a);
  return 1;
}

int app_confirm_undo(app *a)
{
  const char *label;

  if (a->confirm.open)
  {
    return 1;
  }

  label = aud_doc_undo_label(&a->doc);
  if (label == NULL || strcmp(label, AUD_REPAIR_LABEL) != 0)
  {
    return 0; /* an ordinary undo is the safety net, not a thing to guard */
  }

  /*
   * The one undo worth stopping for. It puts the audio back, which is all
   * anybody asked - but the cleaned-up WAV it wrote stays on disk with nothing
   * pointing at it, and it cannot be deleted here because redo still refers to
   * it. See edit/repair.h.
   */
  ask(a, APP_CONFIRM_UNDO, "Undo", "Undo the clean-up?");
  because(a, 0, "The audio comes back as it was recorded.");
  because(a, 0,
          "The " AUD_REPAIR_PREFIX
          "-NNN.wav it wrote stays in your takes folder with nothing");
  because(a, 0, "pointing at it - redo still needs it, so it is not deleted here.");
  return 1;
}

int app_confirm_apply(app *a, double seconds, const char *track)
{
  if (a->confirm.open)
  {
    return 1;
  }

  /*
   * Always asked, unlike the edits. This is the one operation that rewrites
   * audio rather than moving clips over it, and the one that leaves a file
   * behind - so it is a different kind of act, and worth a different answer.
   */
  ask(a, APP_CONFIRM_APPLY, "Clean it up", "Clean up %.1f seconds of %.40s?", seconds,
      track);
  because(a, 0, "This rewrites the audio rather than moving clips about, and");
  because(a, 0, "writes a " AUD_REPAIR_PREFIX "-NNN.wav beside your takes.");
  because(a, 0, "Ctrl+Z puts the recording back.");
  warn_about_redo(a);
  return 1;
}

int app_confirm_quit(app *a)
{
  int recording = a->record_track >= 0;
  int unsaved = a->project_dirty && a->doc.count > 0;

  if (a->confirm.open)
  {
    return 1;
  }

  if (!recording && !unsaved)
  {
    return 0;
  }

  ask(a, APP_CONFIRM_QUIT, "Quit", "Quit with work not saved?");

  if (recording)
  {
    because(a, 0, "A take is still recording. It is a finished WAV either way.");
  }
  if (unsaved)
  {
    /*
     * Not a lie by omission: the edits really are written out on the way past,
     * and saying so is the difference between a dialog that informs and one
     * that only frightens. What it cannot promise is that anybody will find
     * the file, which is why it names it.
     */
    if (a->project_path[0] != '\0')
    {
      because(a, 0, "Your edits are written back to %.60s on the way out.",
              aud_path_basename(a->project_path));
    }
    else
    {
      because(a, 0, "This session has never been saved.");
      because(a, 0,
              "The edits go to recovered" AUD_PROJECT_EXT
              " beside your takes, not nowhere.");
    }
  }

  return 1;
}

/* -- the dialog ------------------------------------------------------------- */

int app_confirm_draw(app *a)
{
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle panel;
  Rectangle cancel;
  Rectangle accept;
  Color tint;
  float y;
  int quit = 0;

  if (!a->confirm.open)
  {
    return 0;
  }

  /* the window dimmed rather than hidden: this is a question about what is
   * behind it, and covering that up would be answering it blind */
  DrawRectangleRec(screen, Fade(BLACK, 0.6f));

  panel.width = CONFIRM_W;
  if (panel.width > screen.width - 2.0f * APP_PAD)
  {
    panel.width = screen.width - 2.0f * APP_PAD;
  }
  panel.height = CONFIRM_PAD * 2.0f + 34.0f + (float)a->confirm.reasons * CONFIRM_ROW +
                 CONFIRM_BUTTON_H + 18.0f;
  panel.x = (screen.width - panel.width) / 2.0f;
  panel.y = (screen.height - panel.height) / 2.0f;
  if (panel.y < APP_PAD)
  {
    panel.y = APP_PAD;
  }

  tint = a->confirm.irreversible ? AUD_UI_RECORD : AUD_UI_WARN;

  DrawRectangleRounded(panel, 12.0f / panel.height, 8, AUD_UI_PANEL);
  DrawRectangleRoundedLines(panel, 12.0f / panel.height, 8, tint);

  aud_ui_text(panel.x + CONFIRM_PAD, panel.y + CONFIRM_PAD, 19, AUD_UI_TEXT,
              a->confirm.title);

  y = panel.y + CONFIRM_PAD + 34.0f;
  for (int i = 0; i < a->confirm.reasons; i++)
  {
    aud_ui_text(panel.x + CONFIRM_PAD, y, 14, AUD_UI_MUTED, a->confirm.reason[i]);
    y += CONFIRM_ROW;
  }

  accept.width = CONFIRM_BUTTON_W;
  accept.height = CONFIRM_BUTTON_H;
  accept.x = panel.x + panel.width - CONFIRM_PAD - accept.width;
  accept.y = panel.y + panel.height - CONFIRM_PAD - accept.height + 6.0f;

  cancel = accept;
  cancel.x = accept.x - 10.0f - cancel.width;

  /*
   * Cancel first in the reading order and lit as the ordinary answer, because
   * it is: a question that got this far is one where carrying on is the choice
   * that needs making rather than the one to fall into.
   */
  if (aud_ui_button(cancel, "Cancel", AUD_UI_ACCENT, 1) || IsKeyPressed(KEY_ESCAPE))
  {
    app_confirm_dismiss(a);
    return 0;
  }

  if (aud_ui_button(accept, a->confirm.accept, tint, 1))
  {
    app_confirm_kind kind = a->confirm.kind;
    app_edit_action action = a->confirm.action;
    long track = a->confirm.track;

    /*
     * Taken down before the action runs, so an action that wants to ask
     * something else - or that fails and says so - is not doing it behind a
     * dialog that is still up.
     */
    app_confirm_dismiss(a);

    switch (kind)
    {
    case APP_CONFIRM_EDIT:
      app_edit_now(a, action);
      break;
    case APP_CONFIRM_CLOSE_TRACK:
      if (track >= 0 && (size_t)track < a->doc.count)
      {
        char name[AUD_TRACK_NAME_MAX];

        snprintf(name, sizeof(name), "%s", a->doc.tracks[track].name);
        if (aud_edit_remove_track(&a->doc, (size_t)track) == 0)
        {
          a->project_dirty = 1;
          app_set_status(a, "closed %.40s", name);
        }
      }
      break;
    case APP_CONFIRM_UNDO:
      app_edit_now(a, APP_EDIT_UNDO);
      break;
    case APP_CONFIRM_APPLY:
      aud_repair_panel_apply(&a->repair, &a->doc, a->take_dir);
      a->project_dirty = 1;
      app_set_status(a, "%s", a->repair.note);
      break;
    case APP_CONFIRM_QUIT:
      quit = 1;
      break;
    case APP_CONFIRM_NONE:
    default:
      break;
    }
  }

  return quit;
}
