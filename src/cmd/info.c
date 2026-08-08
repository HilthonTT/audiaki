/* SPDX-License-Identifier: MIT */
/*
 * --info: report what a finished take came out like.
 *
 * The measuring is take/info.c, which reads a WAV and nothing else. This is the
 * part that decides how the answer is laid out, which depends on how many files
 * were named.
 */
#include "cmd/cmd.h"

#include "take/info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The file --info named, then any that followed it. */
static const char *info_path(const aud_options *opts, int index)
{
  return index == 0 ? opts->input_path : opts->extra_inputs[index - 1];
}

/*
 * One file gets the full report it always did. Several get a row each, because
 * the question behind 'audiaki --info session-*.wav' is which take to keep, and
 * twelve reports in a row answer it worse than twelve lines do.
 */
int aud_cmd_info(const aud_options *opts)
{
  int count = opts->extra_input_count + 1;
  unsigned width = 0;
  int failures = 0;
  int printed = 0;

  if (count == 1)
  {
    aud_info_report report;

    if (aud_info_analyse(opts->input_path, &report) != 0)
    {
      return EXIT_FAILURE;
    }

    if (opts->json)
    {
      aud_info_print_json(stdout, opts->input_path, &report);
    }
    else
    {
      aud_info_print(stdout, opts->input_path, &report);
    }
    return EXIT_SUCCESS;
  }

  for (int i = 0; i < count; i++)
  {
    size_t len = strlen(info_path(opts, i));

    if (len > width)
    {
      width = (unsigned)len;
    }
  }
  width = aud_info_row_width(width);

  if (opts->json)
  {
    fputs("[\n", stdout);
  }

  for (int i = 0; i < count; i++)
  {
    const char *path = info_path(opts, i);
    aud_info_report report;

    /*
     * A file that cannot be read is reported and stepped over. Stopping at the
     * first one would hide the state of every take after it, which is the
     * opposite of what measuring a whole session is for.
     */
    if (aud_info_analyse(path, &report) != 0)
    {
      failures++;
      continue;
    }

    if (opts->json)
    {
      if (printed > 0)
      {
        fputs(",\n", stdout);
      }
      aud_info_print_json(stdout, path, &report);
    }
    else
    {
      if (printed == 0)
      {
        aud_info_print_row_header(stdout, width);
      }
      aud_info_print_row(stdout, path, &report, width);
    }
    printed++;
  }

  if (opts->json)
  {
    fputs("]\n", stdout);
  }

  return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
