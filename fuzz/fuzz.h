/* SPDX-License-Identifier: MIT */
/*
 * fuzz.h - what the fuzz targets share.
 *
 * Three of audiaki's parsers read files it did not write: a project saved by an
 * older version, somebody else's WAV, a take a crash left half way through. All
 * three are reached from a path rather than from a buffer, so a target that
 * wants to hand one arbitrary bytes has to put the bytes in a file first. That
 * is all this is.
 *
 * The file is made once per process and rewritten in place, because a target
 * runs the input millions of times and creating and unlinking a file each time
 * would be most of what is measured.
 */
#ifndef AUDIAKI_FUZZ_H
#define AUDIAKI_FUZZ_H

#include <stddef.h>
#include <stdint.h>

/*
 * The libFuzzer entry point, declared here so replay.c can call it without
 * each target having to export a header of its own.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Put `size` bytes in a file and return its path, or NULL if there is nowhere
 * to put them. The same path every time, rewritten in place, and removed when
 * the process ends.
 */
const char *fuzz_file(const uint8_t *data, size_t size, const char *suffix);

/*
 * An empty file a target may write its own output to, distinct from the one
 * above so a round trip does not land on the input it is still holding.
 * Truncated on each call; NULL if there is nowhere to put it.
 */
const char *fuzz_scratch(const char *suffix);

#endif /* AUDIAKI_FUZZ_H */
