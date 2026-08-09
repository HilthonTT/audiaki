/* SPDX-License-Identifier: MIT */
/*
 * prompt.h - the one question the terminal recorder asks back.
 *
 * The window has a dialog for deciding where a finished take goes; this is the
 * same question with nothing to draw it on. It is deliberately the whole of the
 * terminal's input handling: audiaki is driven by argv, and a recorder that
 * stopped to ask things would be a recorder nobody could script.
 *
 * Questions go to stderr and answers come from stdin, the same split log.h
 * keeps, so a take's name being chosen interactively never lands in the middle
 * of piped output.
 */
#ifndef AUDIAKI_PROMPT_H
#define AUDIAKI_PROMPT_H

#include "util/path.h"

#include <stddef.h>

/*
 * The longest answer a question takes, terminator and newline included. A path
 * plus a little, because a folder is the longest thing anyone types here.
 */
#define AUD_PROMPT_LINE_MAX (AUD_PATH_MAX + 2u)

/*
 * Non-zero when there is a person at a terminal to answer. Both ends have to
 * be one: a question written into a log file, or read from a pipe that will
 * never say anything, is a recording that appears to have hung.
 */
int aud_prompt_available(void);

/*
 * Ask `label`, showing `fallback` as the answer that Enter accepts, and write
 * what was typed into `dst`.
 *
 * Returns 0 with an answer - `fallback` when the line was empty - or -1 at end
 * of input, when the read was interrupted, or when the line was too long for
 * `dst`. A caller that gets -1 should carry on with whatever it would have done
 * without asking: someone pressing Ctrl+C or Ctrl+D at a question is answering
 * "leave it alone", not asking for the take to be dealt with some other way.
 */
int aud_prompt_line(const char *label, const char *fallback, char *dst, size_t size);

#endif /* AUDIAKI_PROMPT_H */
