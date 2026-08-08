/* SPDX-License-Identifier: MIT */
#include "media/ffmpeg.h"

#include "util/log.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define READ_END 0
#define WRITE_END 1

struct FFMPEG
{
  int pipe;
  pid_t pid;
};

/*
 * Write exactly `bytes` from `buf`. write() is allowed to accept less than it
 * was given, and does so routinely on a pipe once ffmpeg falls behind, so a
 * single call per frame would silently corrupt the video.
 */
static int write_all(int fd, const void *buf, size_t bytes)
{
  const unsigned char *p = (const unsigned char *)buf;

  while (bytes > 0)
  {
    ssize_t n = write(fd, p, bytes);

    if (n < 0)
    {
      if (errno == EINTR)
      {
        continue;
      } /* a signal arrived mid-write, not a failure */
      /*
       * EPIPE only says ffmpeg is gone, not why. ffmpeg_end_rendering() is
       * about to reap it and report the actual reason, so leading with a
       * broken pipe here would just bury the useful message.
       */
      if (errno == EPIPE)
      {
        aud_debug("ffmpeg: the pipe closed early, ffmpeg has exited");
      }
      else
      {
        aud_perror("ffmpeg: cannot write a frame to the pipe");
      }
      return -1;
    }
    if (n == 0)
    {
      aud_error("ffmpeg: the pipe accepted no data");
      return -1;
    }

    p += (size_t)n;
    bytes -= (size_t)n;
  }
  return 0;
}

/*
 * ffmpeg reads any argument starting with '-' as an option, so a perfectly
 * legal path like "-take01.wav" would be parsed as flags rather than opened.
 * There is no "--" to end the option list, so the fix is to make the path
 * explicitly relative. Returns `path` itself when it needs no help; otherwise
 * writes "./path" into `buf`.
 */
static const char *safe_path(const char *path, char *buf, size_t size)
{
  if (path == NULL || path[0] != '-')
  {
    return path;
  }

  if ((size_t)snprintf(buf, size, "./%s", path) >= size)
  {
    return NULL;
  } /* too long to disarm; the caller reports it */
  return buf;
}

FFMPEG *ffmpeg_start_rendering(const char *output_path, size_t width, size_t height,
                               size_t fps, const char *sound_file_path)
{
  FFMPEG *ffmpeg;
  int pipefd[2];
  pid_t child;
  char out_buf[PATH_MAX + 3];
  char snd_buf[PATH_MAX + 3];

  /* sound_file_path may be NULL: that is a video with no audio track */
  if (output_path == NULL || width == 0 || height == 0 || fps == 0)
  {
    aud_error("ffmpeg: invalid render parameters");
    return NULL;
  }

  if (sound_file_path != NULL)
  {
    sound_file_path = safe_path(sound_file_path, snd_buf, sizeof(snd_buf));
    if (sound_file_path == NULL)
    {
      aud_error("ffmpeg: the audio path is too long");
      return NULL;
    }
  }

  output_path = safe_path(output_path, out_buf, sizeof(out_buf));
  if (output_path == NULL)
  {
    aud_error("ffmpeg: the output path is too long");
    return NULL;
  }

  if (pipe(pipefd) < 0)
  {
    aud_perror("ffmpeg: cannot create a pipe");
    return NULL;
  }

  child = fork();
  if (child < 0)
  {
    aud_perror("ffmpeg: cannot fork");
    close(pipefd[READ_END]);
    close(pipefd[WRITE_END]);
    return NULL;
  }

  if (child == 0)
  {
    char resolution[64];
    char framerate[64];
    const char *loglevel = aud_log_get_level() >= AUD_LOG_VERBOSE ? "verbose" : "error";

    if (dup2(pipefd[READ_END], STDIN_FILENO) < 0)
    {
      /* the parent's meter may own the current line, so start a fresh one */
      fprintf(stderr, "\nffmpeg child: cannot reopen the pipe as stdin: %s\n",
              strerror(errno));
      _exit(1);
    }
    /*
     * Guarded: if stdin was already closed when audiaki started, pipe() is free
     * to hand back fd 0 as the read end, dup2() is then a no-op, and closing it
     * unconditionally would shut the pipe ffmpeg is about to read from.
     */
    if (pipefd[READ_END] != STDIN_FILENO)
    {
      close(pipefd[READ_END]);
    }
    close(pipefd[WRITE_END]);

    snprintf(resolution, sizeof(resolution), "%zux%zu", width, height);
    snprintf(framerate, sizeof(framerate), "%zu", fps);

    if (sound_file_path != NULL)
    {
      execlp("ffmpeg", "ffmpeg", "-loglevel", loglevel, "-y",

             /* input 0: our frames, arriving on stdin */
             "-f", "rawvideo", "-pix_fmt", "rgba", "-s", resolution, "-r", framerate,
             "-i", "-",

             /* input 1: the audio, which ffmpeg opens for itself */
             "-i", sound_file_path,

             /* -shortest trims the trailing partial frame off the end */
             "-c:v", "libx264", "-vb", "2500k", "-c:a", "aac", "-ab", "200k", "-pix_fmt",
             "yuv420p", "-shortest", output_path,

             NULL);
    }
    else
    {
      /*
       * Silent: one input and -an, rather than muxing a track and muting it.
       * There is no second input to be shorter than the video, so -shortest
       * has nothing to trim and is left off.
       */
      execlp("ffmpeg", "ffmpeg", "-loglevel", loglevel, "-y",

             "-f", "rawvideo", "-pix_fmt", "rgba", "-s", resolution, "-r", framerate,
             "-i", "-",

             "-an", "-c:v", "libx264", "-vb", "2500k", "-pix_fmt", "yuv420p", output_path,

             NULL);
    }

    /* only reached if execlp failed */
    fprintf(stderr, "\nffmpeg child: cannot run ffmpeg: %s\n", strerror(errno));
    _exit(127);
  }

  if (close(pipefd[READ_END]) < 0)
  {
    aud_perror("ffmpeg: cannot close the read end of the pipe");
  }

  /*
   * Without this, ffmpeg exiting early turns the next frame write into a
   * SIGPIPE and kills audiaki before it can report anything useful.
   */
  signal(SIGPIPE, SIG_IGN);

  ffmpeg = malloc(sizeof(*ffmpeg));
  if (ffmpeg == NULL)
  {
    aud_error("ffmpeg: out of memory");
    close(pipefd[WRITE_END]);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    return NULL;
  }

  ffmpeg->pid = child;
  ffmpeg->pipe = pipefd[WRITE_END];
  return ffmpeg;
}

int ffmpeg_send_frame(FFMPEG *ffmpeg, const void *data, size_t width, size_t height)
{
  if (ffmpeg == NULL || data == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  return write_all(ffmpeg->pipe, data, width * height * sizeof(uint32_t));
}

int ffmpeg_send_frame_flipped(FFMPEG *ffmpeg, const void *data, size_t width,
                              size_t height)
{
  const uint32_t *rows = (const uint32_t *)data;

  if (ffmpeg == NULL || data == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  for (size_t y = height; y > 0; y--)
  {
    if (write_all(ffmpeg->pipe, rows + (y - 1) * width, width * sizeof(uint32_t)) != 0)
    {
      return -1;
    }
  }
  return 0;
}

int ffmpeg_end_rendering(FFMPEG *ffmpeg, int cancel)
{
  int pipe_fd;
  pid_t pid;
  int rc = 0;

  if (ffmpeg == NULL)
  {
    return -1;
  }

  pipe_fd = ffmpeg->pipe;
  pid = ffmpeg->pid;
  free(ffmpeg);

  /* closing the pipe is ffmpeg's end-of-stream signal, so it must come first */
  if (close(pipe_fd) < 0)
  {
    aud_perror("ffmpeg: cannot close the write end of the pipe");
  }

  if (cancel)
  {
    kill(pid, SIGKILL);
  }

  for (;;)
  {
    int wstatus = 0;

    if (waitpid(pid, &wstatus, 0) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      aud_perror("ffmpeg: cannot wait for the ffmpeg process");
      return -1;
    }

    if (WIFEXITED(wstatus))
    {
      int status = WEXITSTATUS(wstatus);

      /*
       * 127 is the exit code the child uses when execlp() fails, and cancelling
       * cannot produce it - a killed ffmpeg is WIFSIGNALED. So this is worth
       * saying even when the caller is tearing a failed render down.
       */
      if (status == 127)
      {
        aud_error("could not run ffmpeg");
        aud_info("rendering needs ffmpeg(1) on PATH - install it and try again");
        return -1;
      }

      if (status != 0 && !cancel)
      {
        aud_error("ffmpeg exited with code %d", status);
        rc = -1;
      }
      return rc;
    }

    if (WIFSIGNALED(wstatus))
    {
      if (!cancel)
      {
        aud_error("ffmpeg was terminated by %s", strsignal(WTERMSIG(wstatus)));
        rc = -1;
      }
      return rc;
    }

    /* stopped or continued: keep waiting for it to actually finish */
  }
}
