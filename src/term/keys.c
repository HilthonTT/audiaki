/* SPDX-License-Identifier: MIT */
#include "term/keys.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int aud_keys_open(aud_keys *k)
{
  struct termios raw;

  if (k == NULL)
  {
    return -1;
  }

  memset(k, 0, sizeof(*k));

  /*
   * Both ends have to be a terminal. Reading keys from a pipe would consume
   * whatever was being fed in on stdin, which is somebody else's data, and
   * writing the prompt for them onto a redirected stderr helps nobody.
   */
  if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))
  {
    return -1;
  }

  if (tcgetattr(STDIN_FILENO, &k->saved) != 0)
  {
    return -1;
  }

  raw = k->saved;

  /*
   * Canonical mode holds a key back until Enter, and the echo would land in
   * the middle of the meter line. Nothing else is touched - ISIG in
   * particular stays on, so Ctrl+C interrupts playback the way it interrupts
   * every other command.
   */
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;  /* return whatever has arrived... */
  raw.c_cc[VTIME] = 0; /* ...without waiting for any of it */

  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
  {
    return -1;
  }

  k->active = 1;
  return 0;
}

void aud_keys_close(aud_keys *k)
{
  if (k == NULL || !k->active)
  {
    return;
  }

  /*
   * TCSANOW rather than TCSAFLUSH: keys pressed during the last moment of
   * playback belong to whatever runs next - a shell, or the prompt asking
   * where the take should go - and throwing them away would lose typing the
   * person had every reason to think was going somewhere.
   */
  tcsetattr(STDIN_FILENO, TCSANOW, &k->saved);
  k->active = 0;
}

/*
 * Whether `c` ends a CSI sequence. Everything between the '[' and this is
 * parameters - the digits and semicolons of "\x1b[1;5C" and friends - and none
 * of them change which key it was for the handful of keys read here.
 */
static int csi_final(unsigned char c)
{
  return c >= 0x40u && c <= 0x7eu;
}

/* Decode a cursor key or its relatives, given everything after the escape. */
static aud_key decode_escape(const unsigned char *seq, size_t len, size_t *used)
{
  size_t i;

  /* "\x1bOA" is the same key as "\x1b[A" from a terminal in application mode */
  if (len >= 2u && (seq[0] == '[' || seq[0] == 'O'))
  {
    for (i = 1u; i < len; i++)
    {
      if (!csi_final(seq[i]))
      {
        continue;
      }

      *used = i + 2u; /* the escape and the bytes up to and including the final */
      switch (seq[i])
      {
      case 'A':
        return AUD_KEY_UP;
      case 'B':
        return AUD_KEY_DOWN;
      case 'C':
        return AUD_KEY_RIGHT;
      case 'D':
        return AUD_KEY_LEFT;
      case 'H':
        return AUD_KEY_HOME;
      case 'F':
        return AUD_KEY_END;
      case '~':
        /* "\x1b[1~" and "\x1b[4~", which is how some terminals send them */
        if (seq[1] == '1' || seq[1] == '7')
        {
          return AUD_KEY_HOME;
        }
        if (seq[1] == '4' || seq[1] == '8')
        {
          return AUD_KEY_END;
        }
        return AUD_KEY_OTHER;
      default:
        return AUD_KEY_OTHER;
      }
    }
  }

  /*
   * A bare escape, or the front of a sequence whose tail has not arrived yet.
   * Swallowed rather than guessed at: quitting has a key of its own, and a
   * half-read cursor key that was taken for Escape would stop playback in the
   * middle of a file.
   */
  *used = len;
  return AUD_KEY_OTHER;
}

static aud_key decode(const unsigned char *buf, size_t len, size_t *used)
{
  *used = 1u;

  switch (buf[0])
  {
  case ' ':
    return AUD_KEY_SPACE;
  case 'q':
  case 'Q':
    return AUD_KEY_QUIT;
  case 'n':
  case 'N':
    return AUD_KEY_NEXT;
  case 'p':
  case 'P':
    return AUD_KEY_PREV;
  case 0x1bu:
    return decode_escape(buf + 1, len - 1u, used);
  default:
    return AUD_KEY_OTHER;
  }
}

aud_key aud_keys_poll(aud_keys *k)
{
  size_t used = 0;
  aud_key key;

  if (k == NULL || !k->active)
  {
    return AUD_KEY_NONE;
  }

  if (k->used >= k->have)
  {
    ssize_t got;

    k->used = 0;
    k->have = 0;

    got = read(STDIN_FILENO, k->pending, sizeof(k->pending));
    if (got <= 0)
    {
      /* nothing typed, or a signal landed mid-read; either way, come back */
      return AUD_KEY_NONE;
    }
    k->have = (size_t)got;
  }

  key = decode(k->pending + k->used, k->have - k->used, &used);

  /* a decoder that consumed nothing would spin on the same byte forever */
  if (used == 0)
  {
    used = 1u;
  }
  k->used += used;
  if (k->used > k->have)
  {
    k->used = k->have;
  }

  return key;
}
