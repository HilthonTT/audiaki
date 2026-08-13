/* SPDX-License-Identifier: MIT */
/*
 * --render: mix a saved project down to a WAV.
 *
 * The window's Export button, without the window. Everything it needs - the
 * project format, the clip model and the mixer - is in the portable layer and
 * opens no device, so this runs on a build server, over ssh, or in a loop over
 * a folder of sessions.
 *
 * That is the whole point of a project being a file rather than a thing the
 * editor holds in memory: once a session can be written down, rendering it is
 * something a script can ask for.
 *
 * With --stems it writes one WAV a track instead of one mix, and -o names the
 * set rather than a file. Everything else - the range, the depth, the refusal
 * to overwrite without --force - means the same for a set as for a single file,
 * which is why there is one command here and not two.
 */
#include "cmd/cmd.h"

#include "edit/export.h"
#include "edit/project.h"
#include "take/take.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Whether anything this render would write is already there, naming the first
 * one found. Returns 0 when the ground is clear, -1 when it is not, having said
 * which file is in the way and how to say so on purpose.
 */
static int stems_in_the_way(const aud_doc *d, const aud_export_options *out, int stems)
{
  char path[AUD_PATH_MAX];

  if (!stems)
  {
    if (access(out->path, F_OK) != 0)
    {
      return 0;
    }
    aud_error("%s already exists (pass --force to overwrite)", out->path);
    return -1;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    if (!aud_export_is_stem(d, i))
    {
      continue;
    }
    if (aud_export_stem_path(path, sizeof(path), out->path, i, d->tracks[i].name) != 0)
    {
      continue; /* the export will refuse it too, and say so better than here */
    }
    if (access(path, F_OK) == 0)
    {
      aud_error("%s already exists (pass --force to overwrite)", path);
      return -1;
    }
  }

  return 0;
}

int aud_cmd_render(const aud_options *opts)
{
  aud_doc doc;
  aud_export_options out;
  char derived[AUD_PATH_MAX];
  const char *why = NULL;
  int rc;

  aud_doc_init(&doc, 0);

  if (aud_project_load(&doc, opts->input_path, &why) != 0)
  {
    aud_error("cannot open %s: %s", opts->input_path, why != NULL ? why : "unknown");
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  aud_export_defaults(&out);
  out.path = opts->output_path;
  out.overwrite = opts->overwrite;
  if (opts->export_bits != 0)
  {
    out.bits = opts->export_bits;
  }

  /* default output: the project name with .wav in place of its extension */
  if (out.path == NULL)
  {
    if (aud_take_with_extension(derived, sizeof(derived), opts->input_path, ".wav") != 0)
    {
      aud_error("cannot work out a name to write from '%s'", opts->input_path);
      aud_info("pass the output name with -o");
      aud_doc_free(&doc);
      return EXIT_FAILURE;
    }
    out.path = derived;
  }

  /*
   * A set of stems never lands on the name it was given - every file has a
   * number and a track name on it - so it is only a mixdown that can be asked
   * to write over the project it came from.
   */
  if (!opts->export_stems && strcmp(out.path, opts->input_path) == 0)
  {
    aud_error("%s is both the project and the mix", out.path);
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  if (aud_doc_end(&doc) == 0)
  {
    aud_error("%s holds no audio to mix", opts->input_path);
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  /*
   * The exporter overwrites when asked, so this is what enforces --force - and
   * for stems it is asked of the whole set before any of it is written. Finding
   * out at the fourth file that the third was in the way would leave two behind
   * and no mix, which is the one outcome worth ruling out up front.
   */
  if (!opts->overwrite && stems_in_the_way(&doc, &out, opts->export_stems) != 0)
  {
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  if (opts->export_stems)
  {
    size_t written = 0;

    aud_info("rendering %s: %zu track(s), %.2f s at %u Hz, one WAV a track",
             opts->input_path, doc.count, (double)aud_doc_end(&doc) / doc.rate, doc.rate);

    rc = aud_export_stems(&doc, &out, &written, &why);
    if (rc != 0)
    {
      aud_error("cannot write stems of %s: %s", opts->input_path,
                why != NULL ? why : "unknown");
    }
    else
    {
      /*
       * Named one a line, the way a mixdown names the one file it wrote. Which
       * lanes were left out is worth being able to see, and a count alone does
       * not show it.
       */
      char path[AUD_PATH_MAX];

      for (size_t i = 0; i < doc.count; i++)
      {
        if (!aud_export_is_stem(&doc, i))
        {
          continue;
        }
        if (aud_export_stem_path(path, sizeof(path), out.path, i, doc.tracks[i].name) ==
            0)
        {
          aud_info("wrote %s", path);
        }
      }
      aud_info("%zu stem(s) from %zu track(s)", written, doc.count);
    }

    aud_doc_free(&doc);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  aud_info("rendering %s: %zu track(s), %.2f s at %u Hz -> %s", opts->input_path,
           doc.count, (double)aud_doc_end(&doc) / doc.rate, doc.rate, out.path);

  rc = aud_export_wav(&doc, &out, &why);
  if (rc != 0)
  {
    aud_error("cannot write %s: %s", out.path, why != NULL ? why : "unknown");
  }
  else
  {
    aud_info("wrote %s", out.path);
  }

  aud_doc_free(&doc);
  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
