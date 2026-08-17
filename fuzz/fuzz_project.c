/* SPDX-License-Identifier: MIT */
/*
 * Arbitrary bytes through the project loader.
 *
 * A project file is the one thing audiaki writes that it has to read back, and
 * the one it has already got wrong: a session could be saved into a file that
 * would not reopen. It is also the file most likely to arrive damaged, because
 * it is the one written on the way out of a window that may be closing because
 * something went wrong.
 *
 * The format is lines of keyword and arguments, and the arguments are indices
 * and frame counts - a clip names a source by number and a span by offset and
 * length. Every one of those is an integer read from the file and then used to
 * reach into something, which is why this is worth fuzzing rather than reading.
 *
 * A load that fails must leave nothing behind: the document it was building is
 * discarded and the caller's is untouched, so a project that will not open
 * leaves the session that was already there alone.
 */
#include "fuzz.h"

#include "edit/doc.h"
#include "edit/project.h"

/* the rate a session opens at when the file does not say; any would do */
#define FUZZ_RATE 48000u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  const char *path = fuzz_file(data, size, AUD_PROJECT_EXT);
  const char *why = NULL;
  aud_doc d;

  if (path == NULL)
  {
    return 0;
  }

  aud_doc_init(&d, FUZZ_RATE);

  if (aud_project_load(&d, path, &why) == 0)
  {
    /*
     * It opened, so what came out has to be a document the rest of the editor
     * can be pointed at. Walking it is what says the clip spans the file
     * described are inside the blocks they were laid over.
     */
    for (size_t i = 0; i < d.count; i++)
    {
      (void)aud_track_end(&d.tracks[i]);
    }
    (void)aud_doc_end(&d);

    /*
     * ...and it has to save again. A document that loads and will not save is
     * the bug this target exists for, one step further on.
     */
    {
      const char *back = fuzz_scratch(AUD_PROJECT_EXT);

      if (back != NULL)
      {
        const char *unused = NULL;

        (void)aud_project_save(&d, back, &unused);
      }
    }
  }

  aud_doc_free(&d);
  return 0;
}
