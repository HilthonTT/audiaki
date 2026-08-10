/* SPDX-License-Identifier: MIT */
#include "gui/chooser.h"

#include "util/log.h"
#include "util/path.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* A path plus whatever the front end puts around it. */
#define CHOOSER_OUT_MAX (AUD_PATH_MAX + 64u)

typedef enum
{
  CHOOSER_NONE = 0,
  CHOOSER_ZENITY,
  CHOOSER_KDIALOG,
} chooser_kind;

struct aud_chooser
{
  pid_t pid;
  int fd; /* read end of the child's stdout, non-blocking */
  char out[CHOOSER_OUT_MAX];
  size_t used;
  int done;   /* the child has been reaped */
  int status; /* its exit status, once done */
};

/* Non-zero when `name` is somewhere on PATH. */
static int on_path(const char *name)
{
  const char *path = getenv("PATH");
  char probe[AUD_PATH_MAX];

  if (path == NULL || *path == '\0')
  {
    return 0;
  }

  while (*path != '\0')
  {
    const char *end = strchr(path, ':');
    size_t len = end != NULL ? (size_t)(end - path) : strlen(path);
    size_t name_len = strlen(name);

    /* an empty entry means the working directory, which is not worth probing */
    if (len > 0 && len + 1u + name_len < sizeof(probe))
    {
      memcpy(probe, path, len);
      probe[len] = '/';
      memcpy(probe + len + 1u, name, name_len + 1u);
      if (access(probe, X_OK) == 0)
      {
        return 1;
      }
    }

    if (end == NULL)
    {
      break;
    }
    path = end + 1;
  }
  return 0;
}

/*
 * Which front end to run. The desktop's own comes first when it says which it
 * is - a KDE session gets kdialog, everything else gets zenity - because the
 * point of this is to look like the rest of the desktop rather than to look
 * like GTK.
 */
static chooser_kind pick(void)
{
  const char *forced = getenv("AUDIAKI_FILE_CHOOSER");
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");

  if (forced != NULL && *forced != '\0')
  {
    if (strcmp(forced, "none") == 0)
    {
      return CHOOSER_NONE;
    }
    if (strcmp(forced, "zenity") == 0)
    {
      return on_path("zenity") ? CHOOSER_ZENITY : CHOOSER_NONE;
    }
    if (strcmp(forced, "kdialog") == 0)
    {
      return on_path("kdialog") ? CHOOSER_KDIALOG : CHOOSER_NONE;
    }
    aud_warn("ignoring $AUDIAKI_FILE_CHOOSER=%s: expected zenity, kdialog or none",
             forced);
  }

  if (desktop != NULL && strstr(desktop, "KDE") != NULL && on_path("kdialog"))
  {
    return CHOOSER_KDIALOG;
  }
  if (on_path("zenity"))
  {
    return CHOOSER_ZENITY;
  }
  if (on_path("kdialog"))
  {
    return CHOOSER_KDIALOG;
  }
  return CHOOSER_NONE;
}

int aud_chooser_available(void)
{
  return pick() != CHOOSER_NONE;
}

/* dir and name joined, for the filename a save dialog should open on. */
static void suggest(char *dst, size_t size, const char *dir, const char *name)
{
  int have_dir = dir != NULL && *dir != '\0';

  if (have_dir && name != NULL && *name != '\0')
  {
    snprintf(dst, size, "%s/%s", dir, name);
  }
  else if (have_dir)
  {
    /* the trailing slash is what tells both front ends it is a folder */
    snprintf(dst, size, "%s/", dir);
  }
  else
  {
    snprintf(dst, size, "%s", name != NULL ? name : "");
  }
}

/* Run the front end. Only ever reached in the child, and never returns. */
static void exec_chooser(chooser_kind kind, aud_chooser_mode mode, const char *title,
                         const char *start, const char *filter)
{
  if (kind == CHOOSER_ZENITY)
  {
    char arg_file[AUD_PATH_MAX + 16];
    char arg_title[160];
    char arg_filter[80];

    snprintf(arg_file, sizeof(arg_file), "--filename=%s", start);
    snprintf(arg_title, sizeof(arg_title), "--title=%s", title != NULL ? title : "");
    snprintf(arg_filter, sizeof(arg_filter), "--file-filter=%s",
             filter != NULL ? filter : "*");

    if (mode == AUD_CHOOSER_FOLDER)
    {
      execlp("zenity", "zenity", "--file-selection", "--directory", arg_file, arg_title,
             (char *)NULL);
    }
    else if (mode == AUD_CHOOSER_SAVE)
    {
      execlp("zenity", "zenity", "--file-selection", "--save", "--confirm-overwrite",
             arg_file, arg_title, arg_filter, (char *)NULL);
    }
    else
    {
      execlp("zenity", "zenity", "--file-selection", arg_file, arg_title, arg_filter,
             (char *)NULL);
    }
  }
  else if (kind == CHOOSER_KDIALOG)
  {
    /* kdialog wants "glob|description", and takes the start path positionally */
    char arg_filter[96];

    snprintf(arg_filter, sizeof(arg_filter), "%s|Audio files",
             filter != NULL ? filter : "*");

    if (mode == AUD_CHOOSER_FOLDER)
    {
      execlp("kdialog", "kdialog", "--getexistingdirectory", start, "--title",
             title != NULL ? title : "", (char *)NULL);
    }
    else if (mode == AUD_CHOOSER_SAVE)
    {
      execlp("kdialog", "kdialog", "--getsavefilename", start, arg_filter, "--title",
             title != NULL ? title : "", (char *)NULL);
    }
    else
    {
      execlp("kdialog", "kdialog", "--getopenfilename", start, arg_filter, "--title",
             title != NULL ? title : "", (char *)NULL);
    }
  }

  /* only reached if exec failed, and the parent reads it as a cancel */
  _exit(127);
}

aud_chooser *aud_chooser_start(aud_chooser_mode mode, const char *title, const char *dir,
                               const char *name, const char *filter)
{
  chooser_kind kind = pick();
  aud_chooser *c;
  char start[AUD_PATH_MAX];
  int fds[2];

  if (kind == CHOOSER_NONE)
  {
    return NULL;
  }

  c = calloc(1, sizeof(*c));
  if (c == NULL)
  {
    return NULL;
  }
  /* not the zero calloc leaves: fd 0 is stdin, and closing it would be a bug */
  c->fd = -1;

  if (pipe(fds) != 0)
  {
    free(c);
    return NULL;
  }

  suggest(start, sizeof(start), dir, name);

  c->pid = fork();
  if (c->pid < 0)
  {
    close(fds[0]);
    close(fds[1]);
    free(c);
    return NULL;
  }

  if (c->pid == 0)
  {
    /* the child: its stdout is the pipe, and it inherits stderr to complain on */
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0)
    {
      _exit(127);
    }
    close(fds[1]);
    exec_chooser(kind, mode, title, start, filter);
    _exit(127); /* unreachable; exec_chooser does not return */
  }

  close(fds[1]);
  /*
   * Non-blocking, because this is polled from the render loop: a read that
   * waited for the child would be the freeze this whole arrangement exists to
   * avoid.
   */
  if (fcntl(c->fd = fds[0], F_SETFL, O_NONBLOCK) != 0)
  {
    aud_chooser_close(c);
    return NULL;
  }
  return c;
}

int aud_chooser_poll(aud_chooser *c, char *out, size_t size)
{
  int status;
  pid_t got;

  if (c == NULL)
  {
    return -1;
  }

  /* drain whatever the child has written so far, whether or not it has exited */
  for (;;)
  {
    ssize_t n;

    if (c->used + 1u >= sizeof(c->out))
    {
      break; /* a path longer than this is not one worth having */
    }
    n = read(c->fd, c->out + c->used, sizeof(c->out) - c->used - 1u);
    if (n > 0)
    {
      c->used += (size_t)n;
      continue;
    }
    break;
  }
  c->out[c->used] = '\0';

  if (!c->done)
  {
    got = waitpid(c->pid, &status, WNOHANG);
    if (got == 0)
    {
      return 0; /* still up */
    }
    if (got < 0)
    {
      c->done = 1;
      c->status = 127;
    }
    else
    {
      c->done = 1;
      c->status = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
    }
  }

  /*
   * Cancelled is the ordinary way out of a file dialog, so a non-zero exit is
   * not worth reporting anywhere. Both front ends print nothing in that case.
   */
  if (c->status != 0)
  {
    return -1;
  }

  {
    char *nl = strchr(c->out, '\n');

    if (nl != NULL)
    {
      *nl = '\0';
    }
  }
  if (c->out[0] == '\0')
  {
    return -1;
  }

  snprintf(out, size, "%s", c->out);
  return 1;
}

void aud_chooser_close(aud_chooser *c)
{
  if (c == NULL)
  {
    return;
  }

  if (!c->done && c->pid > 0)
  {
    /*
     * Taking the dialog down with the window rather than leaving it up over a
     * program that is no longer listening for its answer.
     */
    kill(c->pid, SIGTERM);
    waitpid(c->pid, NULL, 0);
  }
  if (c->fd >= 0)
  {
    close(c->fd);
  }
  free(c);
}
