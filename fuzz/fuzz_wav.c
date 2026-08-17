/* SPDX-License-Identifier: MIT */
/*
 * Arbitrary bytes through the WAV reader.
 *
 * audiaki opens WAVs it did not write - a file dragged in from somewhere else,
 * a take an older version stamped, one a crash left with a header describing
 * more audio than is there - and wav.c walks a chunk list to find out what is
 * in them. A chunk list is a length-prefixed format read from a file, which is
 * the shape every buffer overrun in the world is written in: a size field that
 * says more than the file holds, a fmt chunk that stops half way, a bit depth
 * nothing was allocated for.
 *
 * Reading is the point rather than opening, so this decodes as well: a header
 * that parses and then describes frames that are not there is the interesting
 * case and it is only reached by asking for them. Both readers, because they
 * scale and lay out samples differently, and a seek, because it is the one
 * operation that moves the file position by arithmetic on a header field.
 */
#include "fuzz.h"

#include "media/wav.h"

#include <stdlib.h>

/* Enough to reach past any one buffer's worth without decoding a whole file. */
#define FUZZ_FRAMES 512u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  const char *path = fuzz_file(data, size, ".wav");
  wav_reader r;
  float *interleaved;
  float mono[FUZZ_FRAMES];

  if (path == NULL || wav_read_open(&r, path) != 0)
  {
    return 0;
  }

  /*
   * The channel count comes out of the file, so the buffer the caller is told
   * to allocate is sized from something the input controls. That is the
   * contract wav_read_frames() documents, and getting it wrong here would
   * report a bug in this file as a bug in that one.
   */
  interleaved = malloc((size_t)FUZZ_FRAMES * (r.channels > 0 ? r.channels : 1) *
                       sizeof(*interleaved));
  if (interleaved != NULL)
  {
    long got;
    int passes = 0;

    while ((got = wav_read_frames(&r, interleaved, FUZZ_FRAMES)) > 0 && passes < 64)
    {
      passes++;
    }
  }
  free(interleaved);

  /* a jump computed from the header, then reading straight on from wherever it
   * landed - a seek that lands out of bounds must still leave a usable reader */
  if (wav_read_seek(&r, 1) == 0)
  {
    (void)wav_read_mono(&r, mono, FUZZ_FRAMES);
  }
  (void)wav_read_seek(&r, (uint64_t)-1);
  (void)wav_read_mono(&r, mono, FUZZ_FRAMES);
  (void)wav_read_duration(&r);

  wav_read_close(&r);
  return 0;
}
