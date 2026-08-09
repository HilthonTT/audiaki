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
 */
#include "cmd/cmd.h"

#include "edit/export.h"
#include "edit/project.h"
#include "take/take.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

  if (strcmp(out.path, opts->input_path) == 0)
  {
    aud_error("%s is both the project and the mix", out.path);
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  /* the exporter overwrites when asked, so this is what enforces --force */
  if (!opts->overwrite && access(out.path, F_OK) == 0)
  {
    aud_error("%s already exists (pass --force to overwrite)", out.path);
    aud_doc_free(&doc);
    return EXIT_FAILURE;
  }

  if (aud_doc_end(&doc) == 0)
  {
    aud_error("%s holds no audio to mix", opts->input_path);
    aud_doc_free(&doc);
    return EXIT_FAILURE;
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
