/* SPDX-License-Identifier: MIT */
#include "media/ffmpeg.h"

#include "util/bytes.h"
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
       * EPIPE only says ffmpeg is gone, not why. ffmpeg_finish() is
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

/* What ffmpeg is told to be as noisy as, which follows audiaki's own level. */
static const char *ffmpeg_loglevel(void)
{
  return aud_log_get_level() >= AUD_LOG_VERBOSE ? "verbose" : "error";
}

/*
 * Fork, hand the child a pipe as its stdin, and exec `argv` over it. The three
 * kinds of job below differ in nothing else, so this is where all of the
 * fork/dup2/SIGPIPE care lives and each of them only has to say what to run.
 *
 * `argv` is NULL terminated and must begin with "ffmpeg". Returns NULL after
 * reporting the reason through log.h.
 */
static FFMPEG *spawn(char *const argv[])
{
  FFMPEG *ffmpeg;
  int pipefd[2];
  pid_t child;

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

    execvp("ffmpeg", argv);

    /* only reached if execvp failed */
    fprintf(stderr, "\nffmpeg child: cannot run ffmpeg: %s\n", strerror(errno));
    _exit(127);
  }

  if (close(pipefd[READ_END]) < 0)
  {
    aud_perror("ffmpeg: cannot close the read end of the pipe");
  }

  /*
   * Without this, ffmpeg exiting early turns the next write into a SIGPIPE and
   * kills audiaki before it can report anything useful.
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

/* execvp() wants char *const[], and a string literal is not that. */
#define ARG(s) ((char *)(s))

FFMPEG *ffmpeg_start_rendering(const char *output_path, size_t width, size_t height,
                               size_t fps, const char *sound_file_path)
{
  char out_buf[PATH_MAX + 3];
  char snd_buf[PATH_MAX + 3];
  char resolution[64];
  char framerate[64];
  char *argv[32];
  size_t n = 0;

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

  snprintf(resolution, sizeof(resolution), "%zux%zu", width, height);
  snprintf(framerate, sizeof(framerate), "%zu", fps);

  argv[n++] = ARG("ffmpeg");
  argv[n++] = ARG("-loglevel");
  argv[n++] = ARG(ffmpeg_loglevel());
  argv[n++] = ARG("-y");

  /* input 0: our frames, arriving on stdin */
  argv[n++] = ARG("-f");
  argv[n++] = ARG("rawvideo");
  argv[n++] = ARG("-pix_fmt");
  argv[n++] = ARG("rgba");
  argv[n++] = ARG("-s");
  argv[n++] = ARG(resolution);
  argv[n++] = ARG("-r");
  argv[n++] = ARG(framerate);
  argv[n++] = ARG("-i");
  argv[n++] = ARG("-");

  if (sound_file_path != NULL)
  {
    /* input 1: the audio, which ffmpeg opens for itself */
    argv[n++] = ARG("-i");
    argv[n++] = ARG(sound_file_path);
    argv[n++] = ARG("-c:a");
    argv[n++] = ARG("aac");
    argv[n++] = ARG("-ab");
    argv[n++] = ARG("200k");
    /* -shortest trims the trailing partial frame off the end */
    argv[n++] = ARG("-shortest");
  }
  else
  {
    /*
     * Silent: one input and -an, rather than muxing a track and muting it.
     * There is no second input to be shorter than the video, so -shortest has
     * nothing to trim and is left off.
     */
    argv[n++] = ARG("-an");
  }

  argv[n++] = ARG("-c:v");
  argv[n++] = ARG("libx264");
  argv[n++] = ARG("-vb");
  argv[n++] = ARG("2500k");
  argv[n++] = ARG("-pix_fmt");
  argv[n++] = ARG("yuv420p");
  argv[n++] = ARG(output_path);
  argv[n] = NULL;

  return spawn(argv);
}

FFMPEG *ffmpeg_start_encoding(const char *output_path, unsigned rate, unsigned channels,
                              unsigned bits)
{
  char out_buf[PATH_MAX + 3];
  char rate_text[32];
  char channels_text[32];
  char raw[16];
  char *argv[24];
  size_t n = 0;

  if (output_path == NULL || rate == 0 || channels == 0 ||
      (bits != 16u && bits != 24u && bits != 32u))
  {
    aud_error("ffmpeg: invalid encode parameters");
    return NULL;
  }

  output_path = safe_path(output_path, out_buf, sizeof(out_buf));
  if (output_path == NULL)
  {
    aud_error("ffmpeg: the output path is too long");
    return NULL;
  }

  snprintf(raw, sizeof(raw), "s%ule", bits);
  snprintf(rate_text, sizeof(rate_text), "%u", rate);
  snprintf(channels_text, sizeof(channels_text), "%u", channels);

  argv[n++] = ARG("ffmpeg");
  argv[n++] = ARG("-loglevel");
  argv[n++] = ARG(ffmpeg_loglevel());
  argv[n++] = ARG("-y");

  /* the mix, arriving on stdin as the same PCM the WAV writer would have had */
  argv[n++] = ARG("-f");
  argv[n++] = ARG(raw);
  argv[n++] = ARG("-ar");
  argv[n++] = ARG(rate_text);
  argv[n++] = ARG("-ac");
  argv[n++] = ARG(channels_text);
  argv[n++] = ARG("-i");
  argv[n++] = ARG("-");

  /*
   * No codec named. The extension picks both the container and what goes in
   * it, which is ffmpeg's own default and is the rule the video path follows
   * too - and it means a build of ffmpeg with a different encoder for a format
   * uses the one it has rather than the one this file guessed at.
   */
  argv[n++] = ARG(output_path);
  argv[n] = NULL;

  return spawn(argv);
}

int ffmpeg_send_audio(FFMPEG *ffmpeg, const void *data, size_t bytes)
{
  if (ffmpeg == NULL || data == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  return write_all(ffmpeg->pipe, data, bytes);
}

/*
 * Hand `count` canvas words to ffmpeg as the R, G, B, A byte stream it was
 * told to expect. On a little-endian host the words are already in that order
 * in memory, so the buffer goes straight down the pipe; anywhere else they are
 * serialised a bufferful at a time, which costs a copy per frame and is the
 * only thing standing between the visualiser and a big-endian build.
 */
static int write_pixels(int fd, const uint32_t *words, size_t count)
{
#if AUD_HOST_LITTLE_ENDIAN
  return write_all(fd, words, count * sizeof(*words));
#else
  unsigned char staged[1024u * 4u];

  while (count > 0)
  {
    size_t batch = count < sizeof(staged) / 4u ? count : sizeof(staged) / 4u;

    for (size_t i = 0; i < batch; i++)
    {
      aud_wr_u32le(staged + i * 4u, words[i]);
    }
    if (write_all(fd, staged, batch * 4u) != 0)
    {
      return -1;
    }
    words += batch;
    count -= batch;
  }
  return 0;
#endif
}

int ffmpeg_send_frame(FFMPEG *ffmpeg, const void *data, size_t width, size_t height)
{
  if (ffmpeg == NULL || data == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  return write_pixels(ffmpeg->pipe, (const uint32_t *)data, width * height);
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
    if (write_pixels(ffmpeg->pipe, rows + (y - 1) * width, width) != 0)
    {
      return -1;
    }
  }
  return 0;
}

int ffmpeg_finish(FFMPEG *ffmpeg, int cancel)
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
        aud_info("a video, and any export that is not a WAV, needs ffmpeg(1) on "
                 "PATH - install it and try again");
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
