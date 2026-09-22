/* SPDX-License-Identifier: MIT */
#include "edit/ir.h"

#include "audio/resample.h"
#include "edit/load.h"
#include "edit/samples.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define IR_SILENT 1e-5f

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

static char *copy_string(const char *text)
{
  size_t len;
  char *copy;

  if (text == NULL || *text == '\0')
  {
    return NULL;
  }

  len = strlen(text);
  copy = malloc(len + 1u);
  if (copy != NULL)
  {
    memcpy(copy, text, len + 1u);
  }
  return copy;
}

aud_ir *aud_ir_create(const float *data, size_t frames, unsigned channels, unsigned rate,
                      const char *path)
{
  aud_ir *ir;

  if (data == NULL || frames == 0 || channels == 0 || rate == 0)
  {
    return NULL;
  }

  ir = calloc(1, sizeof(*ir));
  if (ir == NULL)
  {
    return NULL;
  }

  ir->data = malloc(frames * channels * sizeof(float));
  if (ir->data == NULL)
  {
    free(ir);
    return NULL;
  }

  memcpy(ir->data, data, frames * channels * sizeof(float));
  ir->refs = 1;
  ir->rate = rate;
  ir->channels = channels;
  ir->frames = frames;
  ir->path = copy_string(path);
  return ir;
}

aud_ir *aud_ir_retain(aud_ir *ir)
{
  if (ir != NULL)
  {
    ir->refs++;
  }
  return ir;
}

void aud_ir_release(aud_ir *ir)
{
  if (ir == NULL || --ir->refs > 0)
  {
    return;
  }

  free(ir->path);
  free(ir->data);
  free(ir);
}

const char *aud_ir_path(const aud_ir *ir)
{
  return ir != NULL && ir->path != NULL ? ir->path : "";
}

static float *resampled(const float *in, size_t frames, unsigned channels, unsigned from,
                        unsigned to, size_t *out_frames)
{
  aud_resampler *rs = aud_resample_create(from, to, channels);
  size_t pad;
  size_t cap;
  size_t made;
  size_t skip;
  float *padded;
  float *out;

  if (rs == NULL)
  {
    return NULL;
  }

  pad = aud_resample_latency(rs) * 2u + 16u;
  padded = calloc((frames + pad) * channels, sizeof(float));
  cap = aud_resample_out_max(rs, frames + pad);
  out = malloc(cap * channels * sizeof(float));
  if (padded == NULL || out == NULL)
  {
    free(padded);
    free(out);
    aud_resample_destroy(rs);
    return NULL;
  }

  memcpy(padded, in, frames * channels * sizeof(float));
  made = aud_resample_run(rs, padded, frames + pad, out, cap);
  skip = (size_t)((double)aud_resample_latency(rs) * to / from + 0.5);
  free(padded);
  aud_resample_destroy(rs);

  if (skip >= made)
  {
    free(out);
    return NULL;
  }

  memmove(out, out + skip * channels, (made - skip) * channels * sizeof(float));
  *out_frames = made - skip;
  return out;
}

static size_t audible_frames(const float *data, size_t frames, unsigned channels)
{
  float loudest = 0.0f;
  size_t end = frames;

  for (size_t i = 0; i < frames * channels; i++)
  {
    float v = fabsf(data[i]);

    if (v > loudest)
    {
      loudest = v;
    }
  }

  while (end > 1u)
  {
    int quiet = 1;

    for (unsigned ch = 0; ch < channels; ch++)
    {
      if (fabsf(data[(end - 1u) * channels + ch]) > loudest * IR_SILENT)
      {
        quiet = 0;
        break;
      }
    }
    if (!quiet)
    {
      break;
    }
    end--;
  }
  return end;
}

static int normalize(float *data, size_t frames, unsigned channels)
{
  double energy = 0.0;
  float scale;

  for (size_t i = 0; i < frames * channels; i++)
  {
    energy += (double)data[i] * data[i];
  }
  energy /= channels;

  if (!(energy > 1e-12))
  {
    return -1;
  }

  scale = (float)(1.0 / sqrt(energy));
  for (size_t i = 0; i < frames * channels; i++)
  {
    data[i] *= scale;
  }
  return 0;
}

aud_ir *aud_ir_load(const char *path, unsigned rate, const char **why)
{
  aud_samples *block;
  unsigned found = 0;
  float *data;
  size_t frames;
  size_t limit;
  unsigned channels;
  aud_ir *ir;

  say(why, NULL);

  if (rate == 0)
  {
    say(why, "there is no sample rate to fit the response to yet");
    return NULL;
  }

  block = aud_edit_read_wav(path, &found, why);
  if (block == NULL)
  {
    return NULL;
  }

  channels = block->channels;
  if (channels > 2u)
  {
    aud_samples_release(block);
    say(why, "an impulse response has one channel or two");
    return NULL;
  }

  frames = block->frames;
  if (found != rate)
  {
    data = resampled(block->data, frames, channels, found, rate, &frames);
  }
  else
  {
    data = malloc(frames * channels * sizeof(float));
    if (data != NULL)
    {
      memcpy(data, block->data, frames * channels * sizeof(float));
    }
  }
  aud_samples_release(block);

  if (data == NULL)
  {
    say(why, "not enough memory for that response");
    return NULL;
  }

  limit = (size_t)(AUD_IR_MAX_SECONDS * rate);
  if (frames > limit)
  {
    frames = limit;
  }
  frames = audible_frames(data, frames, channels);

  if (normalize(data, frames, channels) != 0)
  {
    free(data);
    say(why, "that response is silent");
    return NULL;
  }

  ir = aud_ir_create(data, frames, channels, rate, path);
  free(data);
  if (ir == NULL)
  {
    say(why, "not enough memory for that response");
  }
  return ir;
}
