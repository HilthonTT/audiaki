/* SPDX-License-Identifier: MIT */
/*
 * export.h - the project, back out as a WAV.
 *
 * What every edit has been leading to. It writes the same mix playback plays -
 * see mix.h - so what comes out of the file is what came out of the speakers,
 * which is the one guarantee an editor owes anybody.
 *
 * Written through the same streaming writer the recorder uses, a block at a
 * time, so exporting an hour costs an hour's worth of disk and a period's worth
 * of memory rather than both at once.
 */
#ifndef AUDIAKI_EDIT_EXPORT_H
#define AUDIAKI_EDIT_EXPORT_H

#include "edit/doc.h"

#include <stdint.h>

/* What an export is asked for. */
typedef struct
{
  const char *path;
  unsigned channels; /* 1 or 2; 0 takes the widest track in the project */
  unsigned bits;     /* 16, 24 or 32; 0 means 24, which is what takes are */
  int overwrite;
  /*
   * The range to write. `to` of 0 means "to the end of the project", so the
   * common case - all of it - is two zeros rather than a length that has to be
   * worked out first.
   */
  uint64_t from;
  uint64_t to;
} aud_export_options;

/* Fill `opts` with the defaults described above. */
void aud_export_defaults(aud_export_options *opts);

/*
 * Mix `d` down and write it. Returns 0 on success, or -1 with `*why` set to a
 * static description. `why` may be NULL.
 *
 * A failure leaves no file behind: a half-written export is worse than none,
 * because it looks like a finished one in a directory listing.
 */
int aud_export_wav(const aud_doc *d, const aud_export_options *opts, const char **why);

#endif /* AUDIAKI_EDIT_EXPORT_H */
