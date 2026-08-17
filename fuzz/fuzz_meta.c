/* SPDX-License-Identifier: MIT */
/*
 * Arbitrary bytes through the metadata chunk readers.
 *
 * Reached through fuzz_wav.c as well, but only by an input that first spells a
 * valid RIFF header and a valid fmt chunk and then a LIST or bext of the right
 * length - which a mutation-driven fuzzer will find eventually and this finds
 * on the first input. The two chunk bodies are their own length-prefixed walk
 * inside the outer one, so they are worth reaching directly.
 *
 * The body is split between the two readers rather than given whole to each:
 * they parse different shapes, and a bext is a fixed-layout struct where a LIST
 * is a nest of sub-chunks, so an input that is interesting to one is usually
 * not to the other.
 */
#include "fuzz.h"

#include "take/meta.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  aud_meta_info info;
  size_t half = size / 2u;

  /*
   * Zeroed first, and both readers into the same one: they fill the fields they
   * find and leave the rest alone, which is how wav.c uses them - a take can
   * carry a LIST and a bext and the second is not meant to erase the first.
   */
  memset(&info, 0, sizeof(info));
  aud_meta_read_list(&info, data, half);
  aud_meta_read_bext(&info, data + half, size - half);

  /* and each the whole of it, for the input that is only one of the two */
  memset(&info, 0, sizeof(info));
  aud_meta_read_list(&info, data, size);

  memset(&info, 0, sizeof(info));
  aud_meta_read_bext(&info, data, size);
  return 0;
}
