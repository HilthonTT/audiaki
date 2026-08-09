/* SPDX-License-Identifier: MIT */
/*
 * keys.h - single keypresses read while something is playing.
 *
 * The other half of term/: prompt.h asks a question and waits for a line, this
 * takes a key the moment it is pressed and never waits at all. Playback has a
 * loop of its own to keep fed, so anything that blocked on the keyboard would
 * starve the output; every call here returns immediately, whether or not
 * anything was typed.
 *
 * The terminal is put into a mode where a key arrives without Enter behind it
 * and is not echoed back over the meter line. Signals are deliberately left
 * alone: Ctrl+C must still interrupt, because that is how every other audiaki
 * command stops and playback should be no different.
 *
 * Nothing here is required. With no terminal on the other end - a pipe, a
 * service, a log file - it opens inactive and reports no keys ever, and the
 * caller plays start to finish the way it always did.
 */
#ifndef AUDIAKI_KEYS_H
#define AUDIAKI_KEYS_H

#include <stddef.h>
#include <termios.h>

/*
 * What was pressed, named for what it means rather than for the key: the
 * caller decides that space is pause and that a cursor key is five seconds,
 * and the escape sequences those arrive as are not its business.
 */
typedef enum
{
  AUD_KEY_NONE = 0, /* nothing was typed */
  AUD_KEY_SPACE,
  AUD_KEY_LEFT,
  AUD_KEY_RIGHT,
  AUD_KEY_UP,
  AUD_KEY_DOWN,
  AUD_KEY_HOME,
  AUD_KEY_END,
  AUD_KEY_NEXT,
  AUD_KEY_PREV,
  AUD_KEY_QUIT,
  AUD_KEY_OTHER, /* a key with no meaning here, swallowed rather than echoed */
} aud_key;

typedef struct
{
  int active; /* the terminal really was switched over, and must be put back */
  struct termios saved;

  /*
   * What has been read but not yet decoded. A cursor key is three bytes and a
   * fast pair of presses can arrive in one read, so the bytes are kept and
   * handed out a key at a time rather than a read at a time - otherwise
   * holding an arrow down would lose most of the presses.
   */
  unsigned char pending[32];
  size_t have;
  size_t used;
} aud_keys;

/*
 * Take over the terminal. Returns 0 when keys can be read and -1 when they
 * cannot, which is not a failure worth reporting: a caller that gets -1 should
 * carry on, and aud_keys_poll() will simply never say anything.
 *
 * Safe to close whichever it returned.
 */
int aud_keys_open(aud_keys *k);

/* Put the terminal back as it was. Idempotent, so a cleanup path can run it. */
void aud_keys_close(aud_keys *k);

/*
 * The next key pressed, or AUD_KEY_NONE when none has been. Never blocks.
 *
 * One key per call, so a caller that wants everything typed since the last
 * time round its loop should keep calling until it gets AUD_KEY_NONE.
 */
aud_key aud_keys_poll(aud_keys *k);

#endif /* AUDIAKI_KEYS_H */
