/* SPDX-License-Identifier: MIT */
#include "edit/load.h"

#include "edit/samples.h"
#include "media/wav.h"
#include "util/path.h"

#include <stdio.h>
#include <string.h>

/* Frames decoded per call into the block being filled. */
#define LOAD_CHUNK 8192u

/*
 * A ceiling on one import, so a file with a lying header - or a genuinely
 * enormous one - is refused with a sentence rather than by the machine going
 * to swap. Two hours of stereo float is about 2.5 GiB, which is already past
 * what an in-memory editor should be asked to hold.
 */
#define LOAD_MAX_FRAMES ((uint64_t)2 * 60 * 60 * 96000)

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

aud_samples *aud_edit_read_wav(const char *path, unsigned *out_rate, const char **why)
{
  wav_reader r;
  aud_samples *audio;
  uint64_t done = 0;

  say(why, NULL);

  if (path == NULL)
  {
    say(why, "nothing to load");
    return NULL;
  }

  if (wav_read_open(&r, path) != 0)
  {
    say(why, r.error != NULL ? r.error : "cannot read that file");
    return NULL;
  }

  if (r.frames == 0)
  {
    wav_read_close(&r);
    say(why, "that file holds no audio");
    return NULL;
  }
  if (r.frames > LOAD_MAX_FRAMES)
  {
    wav_read_close(&r);
    say(why, "that file is too long to hold in memory");
    return NULL;
  }

  audio = aud_samples_create(r.channels, (size_t)r.frames);
  if (audio == NULL)
  {
    wav_read_close(&r);
    say(why, "not enough memory to hold that file");
    return NULL;
  }

  while (done < r.frames)
  {
    size_t want = (size_t)(r.frames - done);
    long got;

    if (want > LOAD_CHUNK)
    {
      want = LOAD_CHUNK;
    }

    got = wav_read_frames(&r, audio->data + done * r.channels, want);
    if (got <= 0)
    {
      break; /* a header that promised more than the file holds; keep what came */
    }
    done += (uint64_t)got;
  }

  if (out_rate != NULL)
  {
    *out_rate = r.rate;
  }
  wav_read_close(&r);

  if (done == 0)
  {
    aud_samples_release(audio);
    say(why, "nothing could be decoded from that file");
    return NULL;
  }

  /* what actually arrived, which a truncated file makes shorter than the
   * header claimed - the block is trimmed to it rather than left part silent */
  audio->frames = (size_t)done;
  aud_samples_index(audio);

  /*
   * Where it came from, so a project saved later can point at this file
   * instead of copying its audio. See project.h.
   */
  aud_samples_set_source(audio, path);
  return audio;
}

int aud_edit_load_wav(aud_doc *d, const char *path, const char **why)
{
  aud_samples *audio;
  aud_track *track;
  unsigned rate = 0;
  size_t index;

  say(why, NULL);

  if (d == NULL || path == NULL)
  {
    say(why, "nothing to load");
    return -1;
  }
  if (d->count >= AUD_DOC_MAX_TRACKS)
  {
    say(why, "the project is full");
    return -1;
  }

  audio = aud_edit_read_wav(path, &rate, why);
  if (audio == NULL)
  {
    return -1;
  }

  /*
   * An empty project has no rate of its own yet, so the first thing loaded
   * decides it. After that a mismatch is refused: there is no resampler here,
   * and mixing it in anyway would play it back at the wrong pitch.
   */
  if (d->count == 0)
  {
    d->rate = rate;
  }
  else if (rate != d->rate)
  {
    aud_samples_release(audio);
    say(why, "that file is at a different sample rate");
    return -1;
  }

  track = aud_doc_add_track(d, aud_path_basename(path), audio->channels);
  if (track == NULL)
  {
    aud_samples_release(audio);
    say(why, "the project is full");
    return -1;
  }

  index = d->count - 1u;
  if (aud_track_add(track, audio, 0) != 0)
  {
    aud_doc_remove_track(d, index);
    aud_samples_release(audio);
    say(why, "not enough memory to place that file");
    return -1;
  }

  /* the block belongs to the clip now */
  aud_samples_release(audio);
  return (int)index;
}
