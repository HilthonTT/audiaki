/* SPDX-License-Identifier: MIT */
#ifndef AUDIAKI_EDIT_IR_H
#define AUDIAKI_EDIT_IR_H

#include <stddef.h>

#define AUD_IR_MAX_SECONDS 2.0

typedef struct
{
  int refs;
  unsigned rate;
  unsigned channels;
  size_t frames;
  float *data;
  char *path;
} aud_ir;

aud_ir *aud_ir_create(const float *data, size_t frames, unsigned channels, unsigned rate,
                      const char *path);

aud_ir *aud_ir_load(const char *path, unsigned rate, const char **why);

aud_ir *aud_ir_retain(aud_ir *ir);

void aud_ir_release(aud_ir *ir);

const char *aud_ir_path(const aud_ir *ir);

#endif /* AUDIAKI_EDIT_IR_H */
