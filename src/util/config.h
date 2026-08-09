/* SPDX-License-Identifier: MIT */
/*
 * config.h - the few preferences that outlive an invocation.
 *
 * Almost everything audiaki does is described by the command that asked for it,
 * and a file that quietly changes what a command means is a bad trade. Two
 * things are not like that: where takes are kept, and whether to ask about it
 * afterwards. Both are answered the same way every session by the same person,
 * and neither belongs in the muscle memory of every invocation.
 *
 * The file is read once, before argv is parsed, and every value in it is
 * something the command line can still override. Its absence is the default
 * behaviour rather than an error, so nobody has to create one.
 *
 * No audio and no sound server: the window and the terminal read the same file
 * through the same parser, and the parser is unit tested like the rest of util/.
 */
#ifndef AUDIAKI_CONFIG_H
#define AUDIAKI_CONFIG_H

#include "util/path.h"

#include <stddef.h>

/* Where the file lives when the environment does not say otherwise. */
#define AUD_CONFIG_DIR "audiaki"
#define AUD_CONFIG_FILE "config"

typedef enum
{
  /*
   * Ask when there is someone at a terminal to answer, and carry on quietly
   * when there is not. A pipeline that suddenly waits for a folder name is a
   * pipeline that has hung, so the question is only worth asking of a person.
   */
  AUD_PROMPT_AUTO = 0,
  AUD_PROMPT_NEVER,
  AUD_PROMPT_ALWAYS,
} aud_prompt_mode;

typedef struct
{
  /*
   * Where takes are kept. Empty means the working directory, which is what
   * audiaki did before there was anywhere else to mean.
   */
  char take_dir[AUD_PATH_MAX];
  aud_prompt_mode prompt;
  /*
   * Round-trip latency in milliseconds, for placing an overdub - see
   * take/latency.h. Negative means nothing was said, and it is worked out from
   * the buffers instead.
   *
   * Worth keeping in a file rather than typing: it is a property of the
   * interface and the machine, so it is measured once and true from then on.
   */
  double latency_ms;
} aud_config;

/* The state of a config file that does not exist. */
void aud_config_defaults(aud_config *cfg);

/*
 * Where the config file lives: $AUDIAKI_CONFIG if it is set, otherwise
 * $XDG_CONFIG_HOME/audiaki/config, otherwise ~/.config/audiaki/config.
 *
 * Returns 0 on success, or -1 when there is no home directory to look in -
 * a daemon with no $HOME has no preferences, and that is not a failure.
 */
int aud_config_path(char *dst, size_t size);

/*
 * Fill `cfg` with the defaults and then whatever the config file says. A file
 * that is not there leaves the defaults in place and succeeds.
 *
 * Returns 0 on success, or -1 when a file that does exist could not be read.
 * A line that makes no sense is reported and skipped rather than fatal: one
 * typo should not stop a recording from being made.
 */
int aud_config_load(aud_config *cfg);

/*
 * Apply the config text in `text` to `cfg`, which the caller has already
 * defaulted. `source` names the file in diagnostics.
 *
 * Returns the number of lines that could not be understood, which is zero for
 * a file that is entirely fine.
 */
int aud_config_parse(aud_config *cfg, const char *text, const char *source);

/* "auto", "yes" or "no" for a mode, for the help text and --help output. */
const char *aud_config_prompt_name(aud_prompt_mode mode);

/* The reverse. Returns 0 on success, -1 when `name` is none of them. */
int aud_config_prompt_parse(const char *name, aud_prompt_mode *out);

#endif /* AUDIAKI_CONFIG_H */
