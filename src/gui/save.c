/* SPDX-License-Identifier: MIT */
/*
 * save.c - where a finished take goes.
 *
 * A take is written the moment Record is pressed, under a numbered name, into
 * whatever folder the app was pointed at. That is deliberate: deciding on a
 * name is not something anyone should be doing between hearing the count-in
 * and playing the first note, and a recorder that asked first would lose takes
 * to the question.
 *
 * The question is worth asking afterwards, though, which is what this is. It
 * opens on a file that is already complete and closed, offers the folder it is
 * in and the name it already has, and does nothing at all until one of the two
 * buttons is pressed. Every way out of it that is not "Save" leaves the take
 * exactly where it was recorded: Escape, the close button, and any failure.
 *
 * Both halves live here rather than in screen.c and main.c, the way the rest of
 * the window is split, because the dialog is small and entirely self-contained
 * - the state, the folder listing, the pixels and the move are one thing, and
 * spreading four dozen lines across two files would only make them harder to
 * follow.
 */
#include "gui/app.h"

#include "gui/ui.h"

#include "util/log.h"
#include "util/path.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_PANEL_W 560.0f
#define SAVE_PANEL_H 430.0f
#define SAVE_PAD 24.0f
#define SAVE_ROW_H 32.0f

/* The row that goes up a level, which is always offered first. */
#define SAVE_PARENT ".."

static int compare_rows(const void *a, const void *b)
{
  return strcmp((const char *)a, (const char *)b);
}

/*
 * Fill `s->rows` with the sub-folders of `s->folder`.
 *
 * Folders only. This picks where a take goes, and the files already in a
 * folder are not something to click on - offering them would only invite
 * someone to save one take over another. Hidden ones are left out for the same
 * reason nobody keeps recordings in them.
 */
static void relist(app_save *s)
{
  DIR *dir;
  struct dirent *entry;
  int first; /* the first row that is a real folder, so sorting skips '..' */

  snprintf(s->listed, sizeof(s->listed), "%s", s->folder);
  s->count = 0;
  s->scroll = 0;

  /* '/' is the one folder with nothing above it */
  if (strcmp(s->folder, "/") != 0)
  {
    snprintf(s->rows[s->count++], APP_NAME_MAX, "%s", SAVE_PARENT);
  }
  first = s->count;

  dir = opendir(s->folder);
  if (dir == NULL)
  {
    return;
  }

  while ((entry = readdir(dir)) != NULL && s->count < APP_MAX_FOLDERS)
  {
    char full[AUD_PATH_MAX];

    if (entry->d_name[0] == '.')
    {
      continue;
    }

    /*
     * d_type is not filled in by every filesystem, so a stat is the only
     * answer that holds everywhere. It is one per entry of one folder, at the
     * moment a take stopped, which is nowhere near often enough to matter.
     */
    if (aud_path_join(full, sizeof(full), s->folder, entry->d_name) != 0 ||
        !aud_path_is_dir(full))
    {
      continue;
    }

    snprintf(s->rows[s->count++], APP_NAME_MAX, "%s", entry->d_name);
  }

  closedir(dir);

  /* readdir hands them back in whatever order the filesystem stored them */
  if (s->count - first > 1)
  {
    qsort(s->rows[first], (size_t)(s->count - first), APP_NAME_MAX, compare_rows);
  }

  for (int i = 0; i < s->count; i++)
  {
    s->labels[i] = s->rows[i];
  }
}

void app_save_open(app *a, const char *path, double seconds)
{
  app_save *s = &a->save;

  memset(s, 0, sizeof(*s));
  snprintf(s->take, sizeof(s->take), "%s", path);
  snprintf(s->name, sizeof(s->name), "%s", aud_path_basename(path));
  s->seconds = seconds;

  if (aud_path_dirname(s->folder, sizeof(s->folder), path) != 0)
  {
    snprintf(s->folder, sizeof(s->folder), ".");
  }

  s->focus = APP_SAVE_FIELD_NAME;
  s->open = 1;
  relist(s);
}

void app_save_dismiss(app *a)
{
  app_save *s = &a->save;
  char take[AUD_ENGINE_PATH_MAX];

  if (!s->open)
  {
    return;
  }

  snprintf(take, sizeof(take), "%s", s->take);
  s->open = 0;
  app_finish_take(a, take);
}

/*
 * Put the take where the dialog now says. Returns 0 when it worked, leaving
 * the dialog closed and the video started on wherever it ended up.
 *
 * A failure keeps the dialog open with the reason in it: the file has not
 * moved, and the next answer is a correction rather than a new question.
 */
static int save_confirm(app *a)
{
  app_save *s = &a->save;
  char folder[AUD_PATH_MAX];
  char target[AUD_PATH_MAX];

  if (s->name[0] == '\0')
  {
    snprintf(s->note, sizeof(s->note), "the take needs a name");
    return -1;
  }

  if (strchr(s->name, '/') != NULL)
  {
    snprintf(s->note, sizeof(s->note), "a name cannot contain '/' - that is the folder");
    return -1;
  }

  if (aud_path_expand(folder, sizeof(folder), s->folder) != 0 ||
      aud_path_join(target, sizeof(target), folder, s->name) != 0 ||
      strlen(target) >= AUD_ENGINE_PATH_MAX)
  {
    snprintf(s->note, sizeof(s->note), "that is too long a path");
    return -1;
  }

  /* the file is already there and already called that: nothing to do */
  if (strcmp(target, s->take) == 0)
  {
    app_save_dismiss(a);
    return 0;
  }

  if (aud_path_mkdirs(folder) != 0)
  {
    snprintf(s->note, sizeof(s->note), "cannot use that folder: %s", strerror(errno));
    return -1;
  }

  if (aud_path_move(s->take, target) != 0)
  {
    if (errno == EEXIST)
    {
      snprintf(s->note, sizeof(s->note), "%.180s is already there",
               aud_path_basename(target));
    }
    else
    {
      snprintf(s->note, sizeof(s->note), "cannot save it there: %s", strerror(errno));
    }
    return -1;
  }

  aud_info("stored %s", target);

  /*
   * The engine still has the old name in the status it hands the window every
   * frame, and the status line would go on quoting a file that is not there
   * any more.
   */
  aud_engine_rename_take(a->engine, target);

  s->open = 0;
  app_finish_take(a, target);
  return 0;
}

/* Move the keyboard on, wrapping, the way every other form does. */
static void cycle_focus(app_save *s, int back)
{
  s->focus += back ? -1 : 1;
  if (s->focus < 0)
  {
    s->focus = APP_SAVE_FIELD_COUNT - 1;
  }
  if (s->focus >= APP_SAVE_FIELD_COUNT)
  {
    s->focus = 0;
  }
}

void app_save_draw(app *a)
{
  app_save *s = &a->save;
  Rectangle screen = {0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight()};
  Rectangle panel;
  Rectangle row;
  Rectangle list;
  Rectangle keep;
  Rectangle save;
  float label_w = 66.0f;
  int clicked;

  if (!s->open)
  {
    return;
  }

  /* the window dimmed rather than replaced: the take that was just played is
   * still on the meters behind this, and that is worth seeing */
  DrawRectangleRec(screen, Fade(BLACK, 0.72f));

  panel.width = SAVE_PANEL_W;
  panel.height = SAVE_PANEL_H;
  if (panel.width > screen.width - 2.0f * APP_PAD)
  {
    panel.width = screen.width - 2.0f * APP_PAD;
  }
  if (panel.height > screen.height - 2.0f * APP_PAD)
  {
    panel.height = screen.height - 2.0f * APP_PAD;
  }
  panel.x = (screen.width - panel.width) / 2.0f;
  panel.y = (screen.height - panel.height) / 2.0f;

  DrawRectangleRounded(panel, 12.0f / panel.height, 8, AUD_UI_PANEL);
  DrawRectangleRoundedLines(panel, 12.0f / panel.height, 8, AUD_UI_ACCENT);

  aud_ui_text(panel.x + SAVE_PAD, panel.y + 20.0f, 22, AUD_UI_TEXT, "Keep this take");

  /* how long it was, so the dialog says which take it is asking about */
  {
    char detail[32];

    aud_ui_format_clock(detail, sizeof(detail), s->seconds);
    aud_ui_text_right(panel.x + panel.width - SAVE_PAD, panel.y + 25.0f, 16, AUD_UI_MUTED,
                      detail);
  }

  row.x = panel.x + SAVE_PAD + label_w;
  row.width = panel.width - 2.0f * SAVE_PAD - label_w;
  row.height = SAVE_ROW_H;
  row.y = panel.y + 58.0f;

  aud_ui_text(panel.x + SAVE_PAD, row.y + 8.0f, 18, AUD_UI_MUTED, "name");
  clicked =
      aud_ui_field(row, s->name, sizeof(s->name), s->focus == APP_SAVE_FIELD_NAME, 1);
  if (clicked & AUD_UI_FIELD_CLICKED)
  {
    s->focus = APP_SAVE_FIELD_NAME;
  }
  /*
   * Only bail out of the frame when the dialog is actually gone. A refused
   * save has a reason to put under the fields, and returning here would draw
   * half a dialog for the frame it was refused in.
   */
  if ((clicked & AUD_UI_FIELD_SUBMITTED) && save_confirm(a) == 0)
  {
    return;
  }

  row.y += SAVE_ROW_H + 12.0f;
  aud_ui_text(panel.x + SAVE_PAD, row.y + 8.0f, 18, AUD_UI_MUTED, "folder");
  clicked = aud_ui_field(row, s->folder, sizeof(s->folder),
                         s->focus == APP_SAVE_FIELD_FOLDER, 1);
  if (clicked & AUD_UI_FIELD_CLICKED)
  {
    s->focus = APP_SAVE_FIELD_FOLDER;
  }
  if ((clicked & AUD_UI_FIELD_SUBMITTED) && save_confirm(a) == 0)
  {
    return;
  }

  /*
   * The list follows the field rather than the other way round, so a folder
   * that was typed or pasted is walked into as soon as it exists. Compared as
   * strings because that is what changes: a folder that has grown a new
   * sub-folder since it was listed is not worth a stat every frame.
   */
  if (strcmp(s->listed, s->folder) != 0)
  {
    relist(s);
  }

  list.x = panel.x + SAVE_PAD;
  list.width = panel.width - 2.0f * SAVE_PAD;
  list.y = row.y + SAVE_ROW_H + 18.0f;
  list.height = panel.height - (list.y - panel.y) - SAVE_PAD - 46.0f - 22.0f;

  clicked = aud_ui_list(list, s->labels, s->count, -1, &s->scroll, 1);
  if (clicked >= 0 && clicked < s->count)
  {
    char next[AUD_PATH_MAX];
    int ok;

    if (strcmp(s->rows[clicked], SAVE_PARENT) == 0)
    {
      char here[AUD_PATH_MAX];

      /* through the real path, so ".." out of a relative folder still works */
      ok = aud_path_expand(here, sizeof(here), s->folder) == 0 &&
           aud_path_dirname(next, sizeof(next), here) == 0;
    }
    else
    {
      ok = aud_path_join(next, sizeof(next), s->folder, s->rows[clicked]) == 0;
    }

    if (ok)
    {
      snprintf(s->folder, sizeof(s->folder), "%s", next);
      s->note[0] = '\0';
      relist(s);
    }
  }

  /* why the last attempt did not work, immediately under what it was about */
  if (s->note[0] != '\0')
  {
    aud_ui_text(panel.x + SAVE_PAD, list.y + list.height + 6.0f, 16, AUD_UI_WARN,
                s->note);
  }
  else
  {
    aud_ui_text(panel.x + SAVE_PAD, list.y + list.height + 6.0f, 16, AUD_UI_MUTED,
                "Tab moves between the fields, Enter saves, Esc keeps it where it is");
  }

  save.width = 130.0f;
  save.height = 38.0f;
  save.x = panel.x + panel.width - SAVE_PAD - save.width;
  save.y = panel.y + panel.height - SAVE_PAD - save.height;

  keep = save;
  keep.width = 150.0f;
  keep.x = save.x - 12.0f - keep.width;

  if (aud_ui_button(keep, "Keep here", AUD_UI_MUTED, 1))
  {
    app_save_dismiss(a);
    return;
  }

  if (aud_ui_button(save, "Save", AUD_UI_ACCENT, 1) && save_confirm(a) == 0)
  {
    return;
  }

  /*
   * Last, so the fields have already taken this frame's typing: Tab reaching a
   * field on the frame it moved there would put a tab's worth of nothing in it,
   * and Escape is checked here for the same reason.
   */
  if (IsKeyPressed(KEY_TAB))
  {
    cycle_focus(s, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
  }
  if (IsKeyPressed(KEY_ESCAPE))
  {
    app_save_dismiss(a);
  }
}
