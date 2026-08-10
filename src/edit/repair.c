/* SPDX-License-Identifier: MIT */
#include "edit/repair.h"

#include "media/wav.h"
#include "take/take.h"
#include "util/bytes.h"
#include "util/path.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* What the cleaned-up audio is written at, which is what audiaki records at. */
#define REPAIR_BITS 24u

/* Frames handed to the WAV writer per pass, so a long repair stages little. */
#define REPAIR_CHUNK 8192u

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

/* The lane, when `index` names one and there is a document to look in. */
static aud_track *lane(const aud_doc *d, size_t index)
{
  if (d == NULL || index >= d->count)
  {
    return NULL;
  }
  return &d->tracks[index];
}

int aud_repair_read(const aud_doc *d, size_t index, uint64_t from, uint64_t to,
                    aud_spectral *s)
{
  const aud_track *t = lane(d, index);
  float *frame_buf;
  float *mono;
  size_t window;
  uint64_t span;
  uint64_t step;
  size_t windows;

  if (t == NULL || s == NULL || to <= from || t->channels == 0)
  {
    return -1;
  }

  window = aud_spectral_size(s);
  span = to - from;

  frame_buf = calloc(window * t->channels, sizeof(*frame_buf));
  mono = calloc(window, sizeof(*mono));
  if (frame_buf == NULL || mono == NULL)
  {
    free(frame_buf);
    free(mono);
    return -1;
  }

  /*
   * How far apart the windows sit. A range that only just holds one gets that
   * one; a long one gets AUD_REPAIR_MAX_WINDOWS of them spread from end to
   * end, which is a reading of the take rather than of its opening seconds.
   */
  if (span <= (uint64_t)window)
  {
    windows = 1;
    step = 1;
  }
  else
  {
    uint64_t room = span - (uint64_t)window;

    windows = AUD_REPAIR_MAX_WINDOWS;
    step = room / (uint64_t)(windows - 1u);
    if (step == 0)
    {
      step = 1;
      windows = (size_t)room + 1u;
    }
  }

  aud_spectral_read_begin(s);

  for (size_t w = 0; w < windows; w++)
  {
    uint64_t at = from + (uint64_t)w * step;
    size_t take = window;

    if (at >= to)
    {
      break;
    }
    if (to - at < (uint64_t)take)
    {
      take = (size_t)(to - at);
    }

    aud_track_read(t, at, frame_buf, take);

    for (size_t i = 0; i < take; i++)
    {
      float total = 0.0f;

      for (unsigned c = 0; c < t->channels; c++)
      {
        total += frame_buf[i * t->channels + c];
      }
      mono[i] = total / (float)t->channels;
    }

    aud_spectral_read(s, mono, take);
  }

  aud_spectral_read_end(s);

  free(frame_buf);
  free(mono);
  return aud_spectral_has_reading(s) ? 0 : -1;
}

/*
 * One float sample as REPAIR_BITS of little-endian PCM.
 *
 * Clamped, because a file has to hold something and wrapping round to the
 * opposite polarity would turn a loud passage into a burst of noise. Nothing
 * here should reach full scale that did not arrive at it - the filter only ever
 * takes energy out - but oversubtraction can overshoot by a hair on a transient
 * and a take mastered to the ceiling has no hair to spare.
 */
static void put_sample(unsigned char *dst, float v)
{
  if (v > 1.0f)
  {
    v = 1.0f;
  }
  if (v < -1.0f)
  {
    v = -1.0f;
  }

  aud_wr_s24le(dst, (int32_t)(v * 8388607.0f));
}

/*
 * Write `block` into `dir` under the first free AUD_REPAIR_PREFIX name, and
 * put that name into `path`. Returns 0, or -1 with `*why` set.
 */
static int write_block(const aud_samples *block, unsigned rate, const char *dir,
                       char *path, size_t size, const char **why)
{
  char prefix[AUD_PATH_MAX];
  wav_writer w;
  unsigned char *pcm;
  size_t frame_bytes = (size_t)block->channels * (REPAIR_BITS / 8u);

  if (aud_path_join(prefix, sizeof(prefix), dir, AUD_REPAIR_PREFIX) != 0 ||
      aud_take_next(path, size, prefix) != 0)
  {
    say(why, "there was nowhere to put the cleaned-up audio");
    return -1;
  }

  pcm = malloc(REPAIR_CHUNK * frame_bytes);
  if (pcm == NULL)
  {
    say(why, "not enough memory to write the cleaned-up audio");
    return -1;
  }

  if (wav_open_ex(&w, path, rate, (uint16_t)block->channels, (uint16_t)REPAIR_BITS, 0,
                  NULL, WAV_OPEN_LARGE) != 0)
  {
    say(why, errno == EEXIST ? "a file of that name is already there"
                             : "the cleaned-up audio could not be written");
    free(pcm);
    return -1;
  }

  for (size_t at = 0; at < block->frames;)
  {
    size_t want = block->frames - at;
    size_t bytes;

    if (want > REPAIR_CHUNK)
    {
      want = REPAIR_CHUNK;
    }

    for (size_t i = 0; i < want; i++)
    {
      for (unsigned c = 0; c < block->channels; c++)
      {
        size_t at_sample = (at + i) * block->channels + c;

        put_sample(pcm + (i * block->channels + c) * (REPAIR_BITS / 8u),
                   block->data[at_sample]);
      }
    }

    bytes = want * frame_bytes;
    if (wav_would_overflow(&w, bytes) || wav_write(&w, pcm, bytes) != 0)
    {
      say(why, "the cleaned-up audio could not be written");
      wav_discard(&w);
      free(pcm);
      return -1;
    }

    at += want;
  }

  free(pcm);

  if (wav_close(&w) != 0)
  {
    say(why, "the cleaned-up audio could not be finished");
    return -1;
  }

  return 0;
}

int aud_repair_apply(aud_doc *d, size_t index, uint64_t from, uint64_t to,
                     aud_spectral *s, const char *dir, const char **why)
{
  aud_track *t = lane(d, index);
  aud_samples *block = NULL;
  float *buf = NULL;
  char path[AUD_PATH_MAX];
  size_t length;
  size_t context;
  size_t lead;
  size_t total;
  uint64_t read_at;

  say(why, NULL);

  if (t == NULL || s == NULL || to <= from || t->channels == 0 || d->rate == 0)
  {
    say(why, "there is nothing selected to clean up");
    return -1;
  }

  if (aud_spectral_rate(s) != d->rate)
  {
    say(why, "the analysis was made at another sample rate");
    return -1;
  }

  if (!aud_spectral_would_change(s))
  {
    say(why, "nothing has been taken out of the spectrum yet");
    return -1;
  }

  if (to - from > (uint64_t)SIZE_MAX / (2u * t->channels * sizeof(float)))
  {
    say(why, "that selection is too long to clean up in one go");
    return -1;
  }

  length = (size_t)(to - from);
  context = aud_spectral_context(s);

  /*
   * The run-up: real audio either side, so the transform at the edges of the
   * range sees what is actually next to it rather than the silence past the
   * end of a buffer. Read from as far back as there is track to read.
   */
  read_at = from > (uint64_t)context ? from - context : 0;
  lead = (size_t)(from - read_at);
  total = lead + length + context;

  buf = calloc(total * t->channels, sizeof(*buf));
  if (buf == NULL)
  {
    say(why, "not enough memory to clean up that much audio");
    return -1;
  }

  aud_track_read(t, read_at, buf, total);

  if (aud_spectral_process(s, buf, buf, total, t->channels) != 0)
  {
    say(why, "not enough memory to clean up that much audio");
    free(buf);
    return -1;
  }

  block = aud_samples_create(t->channels, length);
  if (block == NULL)
  {
    say(why, "not enough memory to hold the cleaned-up audio");
    free(buf);
    return -1;
  }

  memcpy(block->data, buf + (size_t)lead * t->channels,
         length * t->channels * sizeof(*buf));
  free(buf);
  buf = NULL;

  aud_samples_index(block);

  if (write_block(block, d->rate, dir, path, sizeof(path), why) != 0)
  {
    aud_samples_release(block);
    return -1;
  }

  if (aud_samples_set_source(block, path) != 0)
  {
    say(why, "not enough memory to remember where that was written");
    aud_samples_release(block);
    return -1;
  }

  /*
   * Nothing below here can fail, which is the point of doing it in this order:
   * everything that could have gone wrong has, and the project has not been
   * touched yet.
   */
  aud_doc_checkpoint(d, AUD_REPAIR_LABEL);

  t = lane(d, index); /* the checkpoint copied the lists; take the pointer again */

  aud_track_delete(t, from, to, 0);
  if (aud_track_place(t, block, 0, length, from, 0, 0) != 0)
  {
    /*
     * Only reachable if the clip list would not grow, and the range has just
     * been emptied so there is room for exactly this one. Undo it rather than
     * leave a hole where the audio was.
     */
    say(why, "the cleaned-up audio would not go back onto the track");
    aud_samples_release(block);
    aud_doc_undo(d);
    return -1;
  }

  aud_samples_release(block); /* the clip holds it now */
  aud_track_tidy(t);
  d->dirty = 1;

  return 0;
}
