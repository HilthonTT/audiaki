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

#include "gui/chooser.h"
#include "gui/ui.h"

#include "edit/export.h"
#include "edit/project.h"
#include "util/log.h"
#include "util/path.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAVE_PANEL_W 560.0f
#define SAVE_PANEL_H 480.0f
#define SAVE_PAD 24.0f
#define SAVE_ROW_H 32.0f

/* The strip of controls between the folder field and the list. */
#define SAVE_TOOLS_H 30.0f

/* The row that goes up a level, which is always offered first. */
#define SAVE_PARENT ".."

/* The dialog's own title, which the system chooser is given as well. */
static const char *save_title(app_save_mode mode);

static int compare_rows(const void *a, const void *b)
{
  return strcmp((const char *)a, (const char *)b);
}

/* Non-zero when `name` ends in .wav, whichever case it was written in. */
static int is_wav(const char *name)
{
  size_t len = strlen(name);
  const char *ext;

  if (len < 5u)
  {
    return 0;
  }
  ext = name + len - 4u;
  return (ext[0] == '.') && (ext[1] == 'w' || ext[1] == 'W') &&
         (ext[2] == 'a' || ext[2] == 'A') && (ext[3] == 'v' || ext[3] == 'V');
}

/* Whether a file is worth offering in this mode, or only folders are. */
static int lists_file(const app_save *s, const char *name)
{
  switch (s->mode)
  {
  /*
   * Filing a take shows the takes already there. It used to show none, on the
   * grounds that they were not something to aim at - but a folder that looks
   * empty is a folder anybody would file take01.wav into twice, and the second
   * one is refused with a message about a file that was never on screen.
   * Better to show what is in the way than to explain it afterwards.
   */
  case APP_SAVE_MODE_KEEP:
  case APP_SAVE_MODE_OPEN:
  case APP_SAVE_MODE_EXPORT:
  case APP_SAVE_MODE_STEMS:
    return is_wav(name);
  /* a session is opened by name, and saved over an existing one knowingly */
  case APP_SAVE_MODE_PROJECT_OPEN:
  case APP_SAVE_MODE_PROJECT_SAVE:
    return aud_project_is_project(name);
  default:
    return 0;
  }
}

/*
 * Fill `s->rows` with what can be clicked in `s->folder`: the sub-folders, and
 * the files this question is about - WAVs, or sessions. A browser that would
 * not show you the file you came for is not one, and one that would not show
 * you the takes already in a folder is asking you to file another one on top of
 * them blind.
 *
 * Dot files and dot folders only when they have been asked for. Hardly anybody
 * keeps recordings in one, so listing them by default would be noise in every
 * home directory; refusing to list them at all would put ~/.local/share out of
 * reach of everything but typing the path.
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
    s->row_is_dir[s->count] = 1;
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

    /*
     * "." and ".." are never rows of their own: the folder you are in is not
     * somewhere to go, and the one above it is offered as SAVE_PARENT at the
     * top whether or not hidden entries are being shown.
     */
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
    {
      continue;
    }
    if (entry->d_name[0] == '.' && !s->show_hidden)
    {
      continue;
    }

    /*
     * d_type is not filled in by every filesystem, so a stat is the only
     * answer that holds everywhere. It is one per entry of one folder, at the
     * moment a take stopped, which is nowhere near often enough to matter.
     */
    if (aud_path_join(full, sizeof(full), s->folder, entry->d_name) != 0)
    {
      continue;
    }

    if (aud_path_is_dir(full))
    {
      s->row_is_dir[s->count] = 1;
    }
    else if (lists_file(s, entry->d_name))
    {
      s->row_is_dir[s->count] = 0;
    }
    else
    {
      continue;
    }

    snprintf(s->rows[s->count++], APP_NAME_MAX, "%s", entry->d_name);
  }

  closedir(dir);

  /*
   * readdir hands them back in whatever order the filesystem stored them, and
   * the folders go above the files so a browser reads the way every other one
   * does. Sorted by the key the rows are drawn with, which is why the flag is
   * folded into it rather than sorted alongside.
   */
  for (int i = first; i < s->count; i++)
  {
    if (s->row_is_dir[i])
    {
      memmove(s->rows[i] + 1, s->rows[i], APP_NAME_MAX - 1u);
      s->rows[i][0] = '\t'; /* sorts before any printable character */
      s->rows[i][APP_NAME_MAX - 1u] = '\0';
    }
  }

  if (s->count - first > 1)
  {
    qsort(s->rows[first], (size_t)(s->count - first), APP_NAME_MAX, compare_rows);
  }

  for (int i = first; i < s->count; i++)
  {
    s->row_is_dir[i] = s->rows[i][0] == '\t';
    if (s->row_is_dir[i])
    {
      memmove(s->rows[i], s->rows[i] + 1, APP_NAME_MAX - 1u);
    }
  }

  for (int i = 0; i < s->count; i++)
  {
    s->labels[i] = s->rows[i];
  }
}

/*
 * The file the dialog's two fields name. Returns 0, or -1 when the two do not
 * make a usable path.
 */
static int save_target(const app_save *s, char *dst, size_t size)
{
  char folder[AUD_PATH_MAX];

  if (aud_path_expand(folder, sizeof(folder), s->folder) != 0 ||
      aud_path_join(dst, size, folder, s->name) != 0)
  {
    return -1;
  }
  return 0;
}

/*
 * What the Play button would play: whatever the fields name, when that is a
 * readable WAV, and otherwise the take being filed. Returns 0 when there is
 * something to hear.
 *
 * The two are the same file when the dialog opens - a take is offered under
 * its own name in its own folder - and they part company the moment the name
 * is edited or a row is clicked. Each is the useful answer where it applies:
 * hear the take you are about to file, or hear the one you are about to be
 * told is in the way.
 */
static int preview_target(const app_save *s, char *dst, size_t size)
{
  if (s->name[0] != '\0' && is_wav(s->name) && strchr(s->name, '/') == NULL &&
      save_target(s, dst, size) == 0 && !aud_path_is_dir(dst) && access(dst, R_OK) == 0)
  {
    return 0;
  }

  if (s->mode == APP_SAVE_MODE_KEEP && s->take[0] != '\0')
  {
    snprintf(dst, size, "%s", s->take);
    return 0;
  }

  return -1;
}

void app_save_open(app *a, const char *path, double seconds)
{
  app_save *s = &a->save;

  memset(s, 0, sizeof(*s));
  s->mode = APP_SAVE_MODE_KEEP;
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

/*
 * The same browser, asking the other question. It opens on the folder takes are
 * kept in, because that is where the file being looked for nearly always is.
 */
void app_open_dialog(app *a)
{
  app_save *s = &a->save;

  memset(s, 0, sizeof(*s));
  s->mode = APP_SAVE_MODE_OPEN;
  s->focus = APP_SAVE_FIELD_NAME;
  s->open = 1;

  if (a->take_dir[0] != '\0')
  {
    snprintf(s->folder, sizeof(s->folder), "%s", a->take_dir);
  }
  else if (a->prefix[0] != '\0' &&
           aud_path_dirname(s->folder, sizeof(s->folder), a->prefix) == 0)
  {
    /* wherever the takes are being written, which with no take_dir is here */
  }
  else
  {
    snprintf(s->folder, sizeof(s->folder), ".");
  }

  relist(s);
}

/*
 * ...and asking where the finished thing should be written. It opens on the
 * take folder with a name derived from the take prefix, so the common answer is
 * already filled in.
 */
void app_export_dialog(app *a, int stems)
{
  app_save *s = &a->save;
  const char *base;

  if (a->doc.count == 0)
  {
    app_set_status(a, "nothing to export yet");
    return;
  }

  memset(s, 0, sizeof(*s));
  s->mode = stems ? APP_SAVE_MODE_STEMS : APP_SAVE_MODE_EXPORT;
  s->focus = APP_SAVE_FIELD_NAME;
  s->open = 1;
  s->seconds = a->doc.rate > 0 ? (double)(aud_doc_has_range(&a->doc)
                                              ? a->doc.sel_end - a->doc.sel_start
                                              : aud_doc_end(&a->doc)) /
                                     a->doc.rate
                               : 0.0;

  base = aud_path_basename(a->prefix);

  /*
   * A set is named for what it is a set of rather than for a mix it is not:
   * "song-stems.wav" is not a file that gets written, it is what every file in
   * the set is named after.
   */
  snprintf(s->name, sizeof(s->name), "%s-%s.wav", base[0] != '\0' ? base : "project",
           stems ? "stems" : "mix");

  if (a->take_dir[0] != '\0')
  {
    snprintf(s->folder, sizeof(s->folder), "%s", a->take_dir);
  }
  else if (aud_path_dirname(s->folder, sizeof(s->folder), a->prefix) != 0)
  {
    snprintf(s->folder, sizeof(s->folder), ".");
  }

  relist(s);
}

/*
 * Where the browser should open when it is about a session rather than a take:
 * beside the project if there is one, and otherwise wherever takes are kept.
 */
static void start_in_project_folder(app *a, app_save *s)
{
  if (a->project_path[0] != '\0' &&
      aud_path_dirname(s->folder, sizeof(s->folder), a->project_path) == 0)
  {
    return;
  }
  if (a->take_dir[0] != '\0')
  {
    snprintf(s->folder, sizeof(s->folder), "%s", a->take_dir);
    return;
  }
  if (aud_path_dirname(s->folder, sizeof(s->folder), a->prefix) != 0)
  {
    snprintf(s->folder, sizeof(s->folder), ".");
  }
}

void app_save_project_as(app *a)
{
  app_save *s = &a->save;
  const char *base;

  memset(s, 0, sizeof(*s));
  s->mode = APP_SAVE_MODE_PROJECT_SAVE;
  s->focus = APP_SAVE_FIELD_NAME;
  s->open = 1;

  /* the name it already has, or one derived from the take prefix */
  if (a->project_path[0] != '\0')
  {
    snprintf(s->name, sizeof(s->name), "%s", aud_path_basename(a->project_path));
  }
  else
  {
    base = aud_path_basename(a->prefix);
    snprintf(s->name, sizeof(s->name), "%s%s", base[0] != '\0' ? base : "session",
             AUD_PROJECT_EXT);
  }

  start_in_project_folder(a, s);
  relist(s);
}

void app_save_project(app *a)
{
  const char *why = NULL;

  /* never saved: there is nothing to write back to, so ask */
  if (a->project_path[0] == '\0')
  {
    app_save_project_as(a);
    return;
  }

  if (aud_project_save(&a->doc, a->project_path, &why) != 0)
  {
    app_set_status(a, "cannot save: %s", why != NULL ? why : "unknown");
    return;
  }

  a->project_dirty = 0;
  app_set_status(a, "saved %.80s", aud_path_basename(a->project_path));
}

void app_open_project_dialog(app *a)
{
  app_save *s = &a->save;

  memset(s, 0, sizeof(*s));
  s->mode = APP_SAVE_MODE_PROJECT_OPEN;
  s->focus = APP_SAVE_FIELD_NAME;
  s->open = 1;

  start_in_project_folder(a, s);
  relist(s);
}

/* What the system chooser should be asking, and about what kind of file. */
static aud_chooser_mode chooser_mode_of(app_save_mode mode)
{
  switch (mode)
  {
  case APP_SAVE_MODE_KEEP:
  case APP_SAVE_MODE_EXPORT:
  case APP_SAVE_MODE_STEMS:
  case APP_SAVE_MODE_PROJECT_SAVE:
    return AUD_CHOOSER_SAVE;
  case APP_SAVE_MODE_OPEN:
  case APP_SAVE_MODE_PROJECT_OPEN:
  default:
    return AUD_CHOOSER_OPEN;
  }
}

static const char *chooser_filter_of(app_save_mode mode)
{
  return (mode == APP_SAVE_MODE_PROJECT_OPEN || mode == APP_SAVE_MODE_PROJECT_SAVE)
             ? "*" AUD_PROJECT_EXT
             : "*.wav";
}

/*
 * Hand the question to the desktop's own chooser, if it has one. The dialog
 * stays open behind it: this is not a different way of answering, it is the
 * same question asked somewhere with bookmarks and a search box, and whatever
 * comes back lands in the two fields as though it had been typed.
 */
static void chooser_begin(app *a)
{
  app_save *s = &a->save;
  char folder[AUD_PATH_MAX];

  if (s->chooser != NULL)
  {
    return; /* one at a time; the button is disabled while one is up */
  }

  if (aud_path_expand(folder, sizeof(folder), s->folder) != 0)
  {
    snprintf(folder, sizeof(folder), "%s", s->folder);
  }

  s->chooser = aud_chooser_start(chooser_mode_of(s->mode), save_title(s->mode), folder,
                                 s->name, chooser_filter_of(s->mode));
  if (s->chooser == NULL)
  {
    snprintf(s->note, sizeof(s->note), "no system file chooser is installed");
  }
}

/*
 * Take the answer, if there is one yet. Called once a frame while a chooser is
 * up, which is what keeps the window drawing while somebody browses.
 */
static void chooser_step(app *a)
{
  app_save *s = &a->save;
  char picked[AUD_PATH_MAX];
  int rc;

  if (s->chooser == NULL)
  {
    return;
  }

  rc = aud_chooser_poll(s->chooser, picked, sizeof(picked));
  if (rc == 0)
  {
    return;
  }

  aud_chooser_close(s->chooser);
  s->chooser = NULL;

  /* cancelled: the dialog is still open and still says what it said before */
  if (rc < 0)
  {
    return;
  }

  /*
   * Split into the two fields rather than acted on directly, so the answer is
   * something that can still be looked at and corrected before either button
   * is pressed. A chooser that saved on the spot would be a second Save with
   * different rules.
   */
  {
    const char *slash = strrchr(picked, '/');
    const char *base = slash != NULL ? slash + 1 : picked;
    size_t dir_len = slash != NULL ? (size_t)(slash - picked) : 0;

    /*
     * A name that will not fit is refused rather than truncated. Silently
     * shortening it would file the take under a name nobody chose, and the
     * chooser is still there to pick another one with.
     */
    if (strlen(base) >= sizeof(s->name) || dir_len >= sizeof(s->folder))
    {
      snprintf(s->note, sizeof(s->note), "that path is too long to use");
      return;
    }

    if (slash != NULL)
    {
      if (dir_len == 0)
      {
        dir_len = 1; /* a file in the root: the folder is "/" */
      }
      memcpy(s->folder, picked, dir_len);
      s->folder[dir_len] = '\0';
    }
    memcpy(s->name, base, strlen(base) + 1u);
  }

  s->note[0] = '\0';
  s->confirmed = 0;
  relist(s);
}

/*
 * The window is going away under the dialog. Not app_save_dismiss(): that
 * answers the question - "keep it here", and a video render off the back of
 * it - and nobody is left to see the answer. This only takes the desktop's
 * chooser down, which is a child process and a pipe rather than a decision.
 */
void app_save_shutdown(app *a)
{
  aud_chooser_close(a->save.chooser);
  a->save.chooser = NULL;
  a->save.open = 0;
}

void app_save_dismiss(app *a)
{
  app_save *s = &a->save;
  char take[AUD_ENGINE_PATH_MAX];

  if (!s->open)
  {
    return;
  }

  /* the chooser is asking about a dialog that is going away; take it with it */
  aud_chooser_close(s->chooser);
  s->chooser = NULL;

  snprintf(take, sizeof(take), "%s", s->take);
  s->open = 0;

  /* an import that was cancelled has no take waiting on an answer */
  if (s->mode == APP_SAVE_MODE_KEEP)
  {
    app_finish_take(a, take);
  }
}

/*
 * The name of the first file an export to `target` would land on, or NULL when
 * the ground is clear. For a mixdown that is `target` itself; for a set of
 * stems it is any one of the files the set is about to become.
 *
 * The answer is what the dialog says out loud, so it is the basename rather
 * than the path - the folder is already on screen above it.
 */
static const char *in_the_way(const app *a, app_save_mode mode, const char *target)
{
  static char taken[AUD_PATH_MAX];

  if (mode != APP_SAVE_MODE_STEMS)
  {
    return access(target, F_OK) == 0 ? aud_path_basename(target) : NULL;
  }

  for (size_t i = 0; i < a->doc.count; i++)
  {
    if (!aud_export_is_stem(&a->doc, i))
    {
      continue;
    }
    if (aud_export_stem_path(taken, sizeof(taken), target, i, a->doc.tracks[i].name) != 0)
    {
      continue; /* the export refuses it too, and says so better than here */
    }
    if (access(taken, F_OK) == 0)
    {
      return aud_path_basename(taken);
    }
  }

  return NULL;
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
    const char *ask = "the take needs a name";

    if (s->mode == APP_SAVE_MODE_OPEN || s->mode == APP_SAVE_MODE_PROJECT_OPEN)
    {
      ask = "pick a file to open";
    }
    else if (s->mode == APP_SAVE_MODE_PROJECT_SAVE)
    {
      ask = "the session needs a name";
    }
    snprintf(s->note, sizeof(s->note), "%s", ask);
    return -1;
  }

  /*
   * A session saved as "riff" is one nothing will offer to open again, so the
   * extension is put back on rather than the answer refused.
   */
  if (s->mode == APP_SAVE_MODE_PROJECT_SAVE && !aud_project_is_project(s->name))
  {
    size_t len = strlen(s->name);

    if (len + sizeof(AUD_PROJECT_EXT) <= sizeof(s->name))
    {
      snprintf(s->name + len, sizeof(s->name) - len, "%s", AUD_PROJECT_EXT);
    }
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

  if (aud_path_is_dir(target))
  {
    snprintf(s->note, sizeof(s->note), "that is a folder - click it to go in");
    return -1;
  }

  if (s->mode == APP_SAVE_MODE_OPEN)
  {
    s->open = 0;
    app_load_track(a, target);
    return 0;
  }

  if (s->mode == APP_SAVE_MODE_PROJECT_OPEN)
  {
    const char *why = NULL;

    /*
     * A failed load leaves the timeline alone - see project.h - so the dialog
     * can simply stay open with the reason in it and let the answer be
     * corrected. Nothing has been lost either way.
     */
    if (aud_project_load(&a->doc, target, &why) != 0)
    {
      snprintf(s->note, sizeof(s->note), "%s", why != NULL ? why : "cannot open that");
      return -1;
    }

    snprintf(a->project_path, sizeof(a->project_path), "%s", target);
    a->project_dirty = 0;
    a->record_track = -1;
    a->last_take_track = -1;
    aud_player_stop(&a->player);
    /* a different session's audio, at possibly a different rate */
    aud_repair_panel_reset(&a->repair);
    s->open = 0;
    app_set_status(a, "opened %.80s: %zu track(s)", aud_path_basename(target),
                   a->doc.count);
    return 0;
  }

  if (s->mode == APP_SAVE_MODE_PROJECT_SAVE)
  {
    const char *why = NULL;

    /* replacing a session is asked about once, like an export */
    if (access(target, F_OK) == 0 && strcmp(target, a->project_path) != 0 &&
        !s->confirmed)
    {
      snprintf(s->note, sizeof(s->note),
               "%.140s is already there - Save again to "
               "replace it",
               aud_path_basename(target));
      s->confirmed = 1;
      return -1;
    }

    if (aud_path_mkdirs(folder) != 0)
    {
      snprintf(s->note, sizeof(s->note), "cannot use that folder: %s", strerror(errno));
      return -1;
    }

    if (aud_project_save(&a->doc, target, &why) != 0)
    {
      snprintf(s->note, sizeof(s->note), "%s", why != NULL ? why : "cannot save that");
      return -1;
    }

    snprintf(a->project_path, sizeof(a->project_path), "%s", target);
    a->project_dirty = 0;
    s->open = 0;
    app_set_status(a, "saved %.80s", aud_path_basename(target));
    return 0;
  }

  if (APP_SAVE_IS_EXPORT(s->mode))
  {
    const char *taken = in_the_way(a, s->mode, target);

    /*
     * Asked before writing rather than refused afterwards. An export is
     * something you do repeatedly to the same name while you get a mix right,
     * so replacing one has to be possible - but not by accident.
     *
     * A set of stems asks about the whole set at once, and names the first file
     * of it that is in the way: finding out at the fourth that the third was
     * there would have left two of them replaced already.
     */
    if (taken != NULL && !s->confirmed)
    {
      snprintf(s->note, sizeof(s->note),
               "%.140s is already there - Export again to "
               "replace it",
               taken);
      s->confirmed = 1;
      return -1;
    }

    if (aud_path_mkdirs(folder) != 0)
    {
      snprintf(s->note, sizeof(s->note), "cannot use that folder: %s", strerror(errno));
      return -1;
    }

    s->open = 0;
    if (s->mode == APP_SAVE_MODE_STEMS)
    {
      app_export_stems(a, target);
    }
    else
    {
      app_export(a, target);
    }
    return 0;
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

/* What the dialog is asking, and what the button that answers it says. */
static const char *save_title(app_save_mode mode)
{
  switch (mode)
  {
  case APP_SAVE_MODE_OPEN:
    return "Open a WAV";
  case APP_SAVE_MODE_EXPORT:
    return "Export a mix";
  case APP_SAVE_MODE_STEMS:
    return "Export the stems";
  case APP_SAVE_MODE_PROJECT_SAVE:
    return "Save the session";
  case APP_SAVE_MODE_PROJECT_OPEN:
    return "Open a session";
  case APP_SAVE_MODE_KEEP:
  default:
    return "Keep this take";
  }
}

static const char *save_action(app_save_mode mode)
{
  switch (mode)
  {
  case APP_SAVE_MODE_OPEN:
  case APP_SAVE_MODE_PROJECT_OPEN:
    return "Open";
  case APP_SAVE_MODE_EXPORT:
  case APP_SAVE_MODE_STEMS:
    return "Export";
  case APP_SAVE_MODE_KEEP:
  case APP_SAVE_MODE_PROJECT_SAVE:
  default:
    return "Save";
  }
}

static const char *save_hint(app_save_mode mode)
{
  switch (mode)
  {
  case APP_SAVE_MODE_OPEN:
    return "click a folder to go in, a file to pick it; Enter opens";
  case APP_SAVE_MODE_EXPORT:
    return "the selection if there is one, the whole project if not";
  case APP_SAVE_MODE_STEMS:
    return "one WAV a track, named after this; they add back up to the mix";
  case APP_SAVE_MODE_PROJECT_SAVE:
    return "the takes stay where they are; this writes what was done to them";
  case APP_SAVE_MODE_PROJECT_OPEN:
    return "opening a session replaces what is on the timeline";
  case APP_SAVE_MODE_KEEP:
  default:
    return "Tab moves between the fields, Enter saves, Esc keeps it here";
  }
}

/*
 * The Play button and what it says it would play, on the right of the tools
 * row. `tools` is that row; `panel` is the dialog, for the right hand edge.
 *
 * Only where there is audio to hear - a session file has none - which is why
 * the whole control is a function rather than a branch inside the drawing.
 */
static void save_draw_preview(app *a, Rectangle panel, Rectangle tools)
{
  app_save *s = &a->save;
  Rectangle play = tools;
  char target[AUD_PATH_MAX];
  char label[128];
  int have = preview_target(s, target, sizeof(target)) == 0;
  int on = aud_preview_playing(&a->preview);

  play.width = 96.0f;
  play.x = panel.x + panel.width - SAVE_PAD - play.width;

  if (aud_ui_button(play, on ? "Stop" : "Play", on ? AUD_UI_WARN : AUD_UI_ACCENT,
                    on || have))
  {
    if (on)
    {
      aud_preview_stop(&a->preview);
    }
    else
    {
      /*
       * One thing at a time. The project and the file the dialog is asking
       * about are two different things to be listening to, and hearing both
       * over each other would tell you nothing about either.
       */
      aud_player_stop(&a->player);

      if (aud_preview_start(&a->preview, target, a->cfg.monitor_device) != 0)
      {
        snprintf(s->note, sizeof(s->note), "cannot play %.100s",
                 aud_path_basename(target));
      }
    }
  }

  if (!on && !have)
  {
    aud_ui_tooltip(play, "there is no take here to hear yet");
    return;
  }

  if (on)
  {
    char at[32];
    char len[32];

    aud_ui_format_clock(at, sizeof(at), aud_preview_position(&a->preview));
    aud_ui_format_clock(len, sizeof(len), aud_preview_length(&a->preview));
    snprintf(label, sizeof(label), "%.40s  %s / %s",
             aud_path_basename(aud_preview_path(&a->preview)), at, len);
  }
  else
  {
    aud_ui_tooltip(play, "hear it before deciding where it goes");
    snprintf(label, sizeof(label), "%.70s", aud_path_basename(target));
  }

  aud_ui_text_right(play.x - 14.0f, tools.y + 7.0f, 16, on ? AUD_UI_TEXT : AUD_UI_MUTED,
                    label);
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
  Rectangle tools;
  Rectangle list;
  Rectangle keep;
  Rectangle save;
  float label_w = 66.0f;
  int clicked;
  int marked = -1;

  if (!s->open)
  {
    /*
     * Whatever was being auditioned goes with the dialog that started it.
     * Here rather than beside each of the half dozen ways out, so a way out
     * added later cannot leave a take playing to an empty window.
     */
    if (aud_preview_playing(&a->preview))
    {
      aud_preview_stop(&a->preview);
    }
    return;
  }

  /*
   * Once a frame, so the window keeps drawing while the desktop's chooser is
   * up. A no-op unless one is - see gui/chooser.h.
   */
  chooser_step(a);

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

  aud_ui_text(panel.x + SAVE_PAD, panel.y + 20.0f, 22, AUD_UI_TEXT, save_title(s->mode));

  /* how long it was, so the dialog says which take it is asking about */
  if (s->mode == APP_SAVE_MODE_KEEP || APP_SAVE_IS_EXPORT(s->mode))
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
  if (clicked & AUD_UI_FIELD_EDITED)
  {
    s->confirmed = 0; /* a different name is a different question */
    s->note[0] = '\0';
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

  /* what is listed on the left, and what can be heard on the right */
  tools.x = panel.x + SAVE_PAD;
  tools.y = row.y + SAVE_ROW_H + 14.0f;
  tools.width = 108.0f;
  tools.height = SAVE_TOOLS_H;

  if (aud_ui_toggle(tools, "Hidden", s->show_hidden, AUD_UI_MUTED, 1))
  {
    s->show_hidden = !s->show_hidden;
    relist(s);
  }
  aud_ui_tooltip(tools, "list dot files and dot folders too");

  /*
   * The way out of this browser and into the desktop's own, which has the
   * bookmarks, the recent places and the search this one will never have.
   * Only offered when there is one to open - a button that did nothing would
   * be worse than the browser it is apologising for.
   */
  if (aud_chooser_available())
  {
    Rectangle browse = tools;
    int up = s->chooser != NULL;

    browse.x = tools.x + tools.width + 10.0f;
    browse.width = 108.0f;

    if (aud_ui_button(browse, up ? "Browsing..." : "Browse...", AUD_UI_MUTED, !up) && !up)
    {
      chooser_begin(a);
    }
    aud_ui_tooltip(browse, up ? "the desktop's file chooser is open"
                              : "pick it in the desktop's own file chooser");
  }

  if (!APP_SAVE_IS_PROJECT(s->mode))
  {
    save_draw_preview(a, panel, tools);
  }

  list.x = panel.x + SAVE_PAD;
  list.width = panel.width - 2.0f * SAVE_PAD;
  list.y = tools.y + SAVE_TOOLS_H + 12.0f;
  list.height = panel.height - (list.y - panel.y) - SAVE_PAD - 46.0f - 22.0f;

  /*
   * A window short enough to squeeze the list out of the panel gets one row of
   * it rather than a rectangle of negative height, which draws as nothing at
   * all and cannot be clicked - a browser that vanishes is harder to recover
   * from than one that is cramped.
   */
  if (list.height < AUD_UI_LIST_ROW)
  {
    list.height = AUD_UI_LIST_ROW;
  }

  /*
   * The row the name field is pointing at, drawn as the current one. With the
   * files listed there is now something for it to point at, and a take about
   * to land on one of them should be able to be seen doing it.
   */
  for (int i = 0; i < s->count; i++)
  {
    if (!s->row_is_dir[i] && strcmp(s->rows[i], s->name) == 0)
    {
      marked = i;
      break;
    }
  }

  clicked = aud_ui_list(list, s->labels, s->count, marked, &s->scroll, 1);
  if (clicked >= 0 && clicked < s->count)
  {
    char next[AUD_PATH_MAX];
    int ok;

    /* a file is what was being looked for; a folder is a step towards it */
    if (!s->row_is_dir[clicked])
    {
      snprintf(s->name, sizeof(s->name), "%s", s->rows[clicked]);
      s->note[0] = '\0';
      s->focus = APP_SAVE_FIELD_NAME;
    }
    else
    {
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
                save_hint(s->mode));
  }

  save.width = 130.0f;
  save.height = 38.0f;
  save.x = panel.x + panel.width - SAVE_PAD - save.width;
  save.y = panel.y + panel.height - SAVE_PAD - save.height;

  keep = save;
  keep.width = 150.0f;
  keep.x = save.x - 12.0f - keep.width;

  if (aud_ui_button(keep, s->mode == APP_SAVE_MODE_KEEP ? "Keep here" : "Cancel",
                    AUD_UI_MUTED, 1))
  {
    app_save_dismiss(a);
    return;
  }

  if (aud_ui_button(save, save_action(s->mode), AUD_UI_ACCENT, 1) && save_confirm(a) == 0)
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
