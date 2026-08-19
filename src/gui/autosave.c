/* SPDX-License-Identifier: MIT */
/*
 * autosave.c - the copy of the session that survives the window dying.
 *
 * The takes were never the thing at risk. Each is a closed WAV the moment it
 * stopped, and a take interrupted by anything short of the power going out has
 * had its header patched. What only exists in memory is everything done to
 * them since - the cuts, the overdub placed against the click, the fades, which
 * lane is which - and until somebody presses ctrl+S that is one segfault away
 * from never having happened.
 *
 * So the window keeps a recovery file: what the session would be if it were
 * saved now, written beside where it would be saved to. A project file is a
 * few kilobytes of text listing which parts of which files sit where - see
 * edit/project.h - so writing one every half minute costs nothing worth
 * measuring, which is the whole reason this is affordable at all.
 *
 * Three rules hold it together, and each of them is there to stop a specific
 * way that automatic saving goes wrong:
 *
 *   It never writes to a file you named. Auto-saving over the session itself
 *   would quietly destroy the one thing a save gives you - a state to go back
 *   to - and "I hadn't saved because I wasn't sure yet" is exactly when it
 *   would hurt. The recovery file is a sidecar and the session is untouched
 *   until you say so.
 *
 *   It is removed the moment the work is safe. A recovery file left lying
 *   about after a clean exit is one that gets offered back the next time, and
 *   an offer that comes up when nothing went wrong is one nobody reads by the
 *   third time. Finding one at startup means a window did not get to say
 *   goodbye, and it means nothing else.
 *
 *   It holds off while a take is running. Not for the cost - it is the same
 *   few kilobytes - but because a block that is still arriving has no file for
 *   a project to point at until the take stops and the WAV is named to it. The
 *   arrangement as it stood before the take started is already on disk, the
 *   take itself is a file from its first period, and the moment it stops the
 *   next write picks up both. What falls out of that is worth having: a
 *   recovery file can only ever refer to takes that finished, so it never
 *   points at a WAV whose header a crash left unpatched.
 */
#include "gui/app.h"

#include "edit/project.h"
#include "util/log.h"
#include "util/path.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int app_recovery_path(const app *a, char *dst, size_t size)
{
  if (a == NULL || dst == NULL || size == 0)
  {
    return -1;
  }

  if (a->project_path[0] != '\0')
  {
    int n = snprintf(dst, size, "%s%s", a->project_path, APP_RECOVER_EXT);

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
  }

  /*
   * Nothing to sit beside, so it goes where the takes go - the same folder the
   * window writes the edits to when it closes without a name for them.
   */
  return aud_path_place(dst, size, a->take_dir, APP_RECOVER_UNNAMED);
}

/* When `path` was last written. Returns 0, or -1 when there is no such file. */
static int file_time(const char *path, time_t *out)
{
  struct stat st;

  if (path == NULL || path[0] == '\0' || stat(path, &st) != 0)
  {
    return -1;
  }

  *out = st.st_mtime;
  return 0;
}

/* A time of day, for saying which afternoon's work this was. */
static void say_when(char *dst, size_t size, time_t when)
{
  struct tm parts;

  if (localtime_r(&when, &parts) == NULL || strftime(dst, size, "%H:%M", &parts) == 0)
  {
    snprintf(dst, size, "earlier");
  }
}

/* Take the recovery file away, if this window is keeping one. */
static void drop(app *a)
{
  if (a->recovery_path[0] != '\0')
  {
    remove(a->recovery_path);
    a->recovery_path[0] = '\0';
  }

  /*
   * So the next edit is written straight away rather than in half a minute.
   * The window that has just been saved is the one where the next thing typed
   * is the only unsaved work there is, and it should not be the least
   * protected.
   */
  a->recovery_at = 0.0;
}

void app_autosave_step(app *a, double now)
{
  char want[AUD_PATH_MAX];
  const char *why = NULL;
  int somewhere;

  if (a == NULL)
  {
    return;
  }

  somewhere = app_recovery_path(a, want, sizeof(want)) == 0;

  /*
   * The work is safe, or it is being kept somewhere else now - a Save As moves
   * where this session's recovery belongs, and the file at the old place
   * describes a session nothing will look for there.
   */
  if (a->recovery_path[0] != '\0' &&
      (!a->project_dirty || !somewhere || strcmp(a->recovery_path, want) != 0))
  {
    drop(a);
  }

  if (!a->project_dirty || a->doc.count == 0 || !somewhere)
  {
    return;
  }

  /* a take still arriving has no file for a project to refer to yet */
  if (a->record_track >= 0)
  {
    return;
  }

  /* `recovery_at` of zero is "nothing written since the last clean state",
   * which is the one case worth writing immediately rather than waiting */
  if (a->recovery_at > 0.0 && now - a->recovery_at < APP_AUTOSAVE_SECONDS)
  {
    return;
  }

  if (aud_project_save(&a->doc, want, &why) != 0)
  {
    /*
     * Held off for the usual interval rather than retried every frame, and
     * said once rather than every time: a folder that cannot be written to
     * will not start being writable between two frames, and a status line
     * repeating itself twice a minute is one nobody reads.
     */
    a->recovery_at = now;
    if (!a->recovery_failed)
    {
      a->recovery_failed = 1;
      aud_warn("cannot keep a recovery file: %s", why != NULL ? why : "unknown");
      app_set_status(a, "cannot keep a recovery file: %.120s",
                     why != NULL ? why : "unknown");
    }
    return;
  }

  snprintf(a->recovery_path, sizeof(a->recovery_path), "%s", want);
  a->recovery_at = now;
  a->recovery_failed = 0;
}

void app_recover(app *a)
{
  char path[AUD_PATH_MAX];
  char when[32];
  const char *why = NULL;
  time_t left;
  time_t saved;

  if (a == NULL || app_recovery_path(a, path, sizeof(path)) != 0)
  {
    return;
  }

  if (file_time(path, &left) != 0)
  {
    return; /* the ordinary case: the last window closed properly */
  }

  /*
   * Older than the session it shadows. That means the session was saved after
   * this was written and the window then died before tidying up, so what is
   * here is a state that has already been superseded - offering it back would
   * be offering to undo a save.
   */
  if (a->project_path[0] != '\0' && file_time(a->project_path, &saved) == 0 &&
      left <= saved)
  {
    remove(path);
    return;
  }

  /*
   * Something was asked for on the command line and there is no project for
   * this to be the recovery of, so it is somebody else's unfinished business
   * rather than this window's. Left where it is, to be picked up by a window
   * that comes up empty.
   */
  if (a->project_path[0] == '\0' && a->doc.count > 0)
  {
    return;
  }

  if (aud_project_load(&a->doc, path, &why) != 0)
  {
    aud_warn("cannot open the recovery file %s: %s", path, why != NULL ? why : "unknown");
    app_set_status(a, "a recovery file was found but will not open: %.100s",
                   why != NULL ? why : "unknown");
    return;
  }

  /*
   * Opened rather than asked about, because there is nothing here to lose:
   * what was on disk is still on disk, and the way back to it is to open it.
   * Kept dirty, though, and said out loud - the session in front of you is not
   * what the file holds, and finding that out at the next save would be
   * finding it out too late.
   */
  say_when(when, sizeof(when), left);
  snprintf(a->recovery_path, sizeof(a->recovery_path), "%s", path);
  a->project_dirty = 1;

  if (a->project_path[0] != '\0')
  {
    aud_info("recovered unsaved edits to %s from %s", a->project_path, when);
    app_set_status(a, "recovered edits to %.60s from %s - save to keep them",
                   aud_path_basename(a->project_path), when);
  }
  else
  {
    aud_info("recovered unsaved edits from %s (%s)", when, path);
    app_set_status(a, "recovered unsaved edits from %s - save them to keep them", when);
  }
}

void app_autosave_done(app *a, int saved)
{
  if (a == NULL)
  {
    return;
  }

  /*
   * Only once the edits are somewhere they will be found. A shutdown save that
   * failed leaves this file as the only copy there is, and taking it away
   * because the window is closing would be throwing the work away at exactly
   * the moment it was promised not to be.
   */
  if (saved || !a->project_dirty || a->doc.count == 0)
  {
    drop(a);
  }
}
