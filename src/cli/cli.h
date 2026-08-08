/* SPDX-License-Identifier: MIT */
/*
 * cli.h - turning argv into an aud_options.
 *
 * Kept free of any audio system's types so the option handling can be tested on
 * its own. What it produces is described by options.h, which src/cmd reads; the
 * parser and the commands meet there and nowhere else.
 *
 * cli.c is the option table and the parse loop. usage.c is the help text, which
 * is long enough that keeping it beside the table made both harder to read.
 */
#ifndef AUDIAKI_CLI_H
#define AUDIAKI_CLI_H

#include "options.h"

#include <stdio.h>

/* exit code used for malformed invocations, matching common CLI convention */
#define CLI_EXIT_USAGE 2

/* Populate `opts` with defaults, honouring the AUDIAKI_DEVICE environment. */
void cli_defaults(aud_options *opts);

/*
 * Parse argv into `opts`. Returns 0 when the caller should proceed, or a
 * process exit code (CLI_EXIT_USAGE) when the invocation was rejected.
 */
int cli_parse(int argc, char **argv, aud_options *opts);

void cli_print_usage(FILE *out);
void cli_print_version(FILE *out);

#endif /* AUDIAKI_CLI_H */
