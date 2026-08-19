/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "edit/export.h"
#include "edit/mix.h"
#include "media/wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_path[256];

/* A track of `frames` frames holding `value` in every sample. */
static aud_track *flat(aud_doc *d, unsigned channels, size_t frames, float value)
{
  aud_samples *s = aud_samples_create(channels, frames);
  aud_track *t = aud_doc_add_track(d, "t", channels);

  for (size_t i = 0; i < frames * channels; i++)
  {
    s->data[i] = value;
  }
  aud_track_add(t, s, 0);
  aud_samples_release(s);
  return t;
}

TEST(tracks_add_up)
{
  aud_doc d;
  aud_mixer m;
  float out[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.25f);
  flat(&d, 1, 100, 0.25f);

  aud_mix_init(&m, 4);
  CHECK_EQ_INT(aud_mix_read(&m, &d, 0, out, 4, 2), 0);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6);
  CHECK_EQ_DBL(out[1], 0.5, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(a_clip_gain_reaches_the_mix_the_way_a_fader_does)
{
  aud_doc d;
  aud_mixer m;
  float out[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.25f);
  CHECK_EQ_INT(aud_track_gain_scale(&d.tracks[0], 0, 100, 2.0f), 0);

  aud_mix_init(&m, 4);
  CHECK_EQ_INT(aud_mix_read(&m, &d, 0, out, 4, 2), 0);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6);

  /* and it multiplies with the track's own fader rather than replacing it */
  d.tracks[0].gain = 0.5f;
  CHECK_EQ_INT(aud_mix_read(&m, &d, 0, out, 4, 2), 0);
  CHECK_EQ_DBL(out[0], 0.25, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(a_mono_track_reaches_both_sides)
{
  aud_doc d;
  aud_mixer m;
  float out[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.5f);

  aud_mix_init(&m, 4);
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6);
  CHECK_EQ_DBL(out[1], 0.5, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(gain_scales_and_mute_silences)
{
  aud_doc d;
  aud_mixer m;
  float out[8];
  aud_track *t;

  aud_doc_init(&d, 44100);
  t = flat(&d, 1, 100, 0.5f);

  t->gain = 0.5f;
  aud_mix_init(&m, 4);
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.25, 1e-6);

  t->muted = 1;
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.0, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(solo_silences_everything_that_is_not)
{
  aud_doc d;
  aud_mixer m;
  float out[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.25f);
  flat(&d, 1, 100, 0.25f);

  aud_mix_init(&m, 4);
  d.tracks[0].soloed = 1;
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.25, 1e-6); /* one of the two, not both */

  CHECK_EQ_INT(aud_mix_audible(&d, &d.tracks[0]), 1);
  CHECK_EQ_INT(aud_mix_audible(&d, &d.tracks[1]), 0);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(pan_moves_a_track_without_making_it_louder)
{
  aud_doc d;
  aud_mixer m;
  float out[8];
  aud_track *t;

  aud_doc_init(&d, 44100);
  t = flat(&d, 1, 100, 0.5f);

  aud_mix_init(&m, 4);

  t->pan = -1.0f; /* hard left */
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6);
  CHECK_EQ_DBL(out[1], 0.0, 1e-6);

  t->pan = 1.0f;
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.0, 1e-6);
  CHECK_EQ_DBL(out[1], 0.5, 1e-6);

  t->pan = 0.0f;
  aud_mix_read(&m, &d, 0, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.5, 1e-6); /* centre is unity, not attenuated */

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(a_gap_and_the_space_past_the_end_are_silent)
{
  aud_doc d;
  aud_mixer m;
  float out[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.5f);
  aud_track_delete(&d.tracks[0], 40, 60, 0);

  aud_mix_init(&m, 4);
  aud_mix_read(&m, &d, 45, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.0, 1e-6);

  aud_mix_read(&m, &d, 500, out, 4, 2);
  CHECK_EQ_DBL(out[0], 0.0, 1e-6);

  aud_mix_free(&m);
  aud_doc_free(&d);
}

TEST(an_export_reads_back_as_what_was_mixed)
{
  aud_doc d;
  wav_reader r;
  aud_export_options opts;
  const char *why = NULL;
  float frames[8];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 1000, 0.5f);

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.overwrite = 1;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT(r.rate, 44100);
  CHECK_EQ_INT(r.channels, 1); /* the widest track was mono, so the file is */
  CHECK_EQ_INT(r.bits, 24);
  CHECK_EQ_INT((int)r.frames, 1000);

  CHECK_EQ_INT((int)wav_read_frames(&r, frames, 4), 4);
  CHECK_EQ_DBL(frames[0], 0.5, 1e-4);
  wav_read_close(&r);

  remove(g_path);
  aud_doc_free(&d);
}

TEST(an_export_of_the_selection_writes_only_that)
{
  aud_doc d;
  wav_reader r;
  aud_export_options opts;

  aud_doc_init(&d, 1000);
  flat(&d, 1, 1000, 0.5f);

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.overwrite = 1;
  opts.from = 100;
  opts.to = 300;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, NULL), 0);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT((int)r.frames, 200);
  wav_read_close(&r);

  remove(g_path);
  aud_doc_free(&d);
}

TEST(an_export_that_refuses_leaves_no_file)
{
  aud_doc d;
  aud_export_options opts;
  const char *why = NULL;

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.5f);

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.bits = 7;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(why != NULL);
  CHECK(access(g_path, F_OK) != 0);

  /* and an empty range is refused rather than writing a header of nothing */
  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.from = 50;
  opts.to = 50;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(access(g_path, F_OK) != 0);

  /* as is a width the mixer has no pan law for */
  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.channels = 6;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(access(g_path, F_OK) != 0);

  aud_doc_free(&d);
}

/* Where stem `index` of a set based at g_path lands, for the checks below. */
static const char *stem_path(size_t index, const char *name)
{
  static char path[512];

  if (aud_export_stem_path(path, sizeof(path), g_path, index, name) != 0)
  {
    path[0] = '\0';
  }
  return path;
}

/*
 * The property the whole feature rests on: the stems, added back together the
 * way anything else would add them, are the mixdown. Gains and pans that differ
 * per track, so a stem that ignored either would be caught.
 */
TEST(stems_add_up_to_the_mix)
{
  aud_doc d;
  aud_export_options opts;
  wav_reader r;
  const char *why = NULL;
  size_t written = 0;
  float mixdown[64];
  float one[64];
  float sum[64];

  aud_doc_init(&d, 44100);
  flat(&d, 1, 500, 0.5f);
  flat(&d, 1, 500, 0.25f);

  /* by index rather than by the pointer flat() returned: the list moves */
  snprintf(d.tracks[0].name, sizeof(d.tracks[0].name), "Rhythm");
  snprintf(d.tracks[1].name, sizeof(d.tracks[1].name), "Lead");
  d.tracks[0].gain = 0.8f;
  d.tracks[0].pan = -0.5f;
  d.tracks[1].gain = 1.5f;
  d.tracks[1].pan = 0.75f;

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.channels = 2;
  opts.overwrite = 1;
  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), 0);
  CHECK_EQ_INT(aud_export_stems(&d, &opts, &written, &why), 0);
  CHECK_EQ_INT((int)written, 2);

  CHECK_EQ_INT(wav_read_open(&r, g_path), 0);
  CHECK_EQ_INT((int)wav_read_frames(&r, mixdown, 32), 32);
  wav_read_close(&r);

  memset(sum, 0, sizeof(sum));
  for (size_t i = 0; i < 2; i++)
  {
    const char *path = stem_path(i, d.tracks[i].name);

    CHECK_EQ_INT(wav_read_open(&r, path), 0);
    CHECK_EQ_INT(r.channels, 2); /* the set agrees on width, or it will not line up */
    CHECK_EQ_INT(r.rate, 44100);
    CHECK_EQ_INT((int)r.frames, 500);
    CHECK_EQ_INT((int)wav_read_frames(&r, one, 32), 32);
    wav_read_close(&r);

    for (size_t f = 0; f < 64; f++)
    {
      sum[f] += one[f];
    }
    remove(path);
  }

  for (size_t f = 0; f < 64; f++)
  {
    CHECK_EQ_DBL(sum[f], mixdown[f], 1e-4);
  }

  /* and the two sides really did differ, so the check above had something to do */
  CHECK(fabs((double)mixdown[0] - (double)mixdown[1]) > 0.01);

  remove(g_path);
  aud_doc_free(&d);
}

TEST(a_stem_is_named_after_its_track)
{
  char path[512];

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "mix.wav", 0, "Rhythm"), 0);
  CHECK(strcmp(path, "mix-01-Rhythm.wav") == 0);

  /* the base keeps its folder, and loses only its own extension */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "a.b/take", 9, "Lead"), 0);
  CHECK(strcmp(path, "a.b/take-10-Lead.wav") == 0);

  /* a name that would reach out of the folder cannot */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "mix.wav", 0, "../Gtr / DI"), 0);
  CHECK(strcmp(path, "mix-01-Gtr-DI.wav") == 0);

  /* and one that leaves nothing usable falls back to the number alone */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "mix.wav", 2, "///"), 0);
  CHECK(strcmp(path, "mix-03.wav") == 0);

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "mix.wav", 3, ""), 0);
  CHECK(strcmp(path, "mix-04.wav") == 0);

  /* a buffer too small to hold the answer is refused rather than truncated */
  CHECK_EQ_INT(aud_export_stem_path(path, 8, "mix.wav", 0, "Rhythm"), -1);
}

TEST(stems_skip_what_the_mix_cannot_hear)
{
  aud_doc d;
  aud_export_options opts;
  size_t written = 0;
  const char *why = NULL;

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.5f);
  flat(&d, 1, 100, 0.5f);
  aud_doc_add_track(&d, "empty", 1); /* a lane with nothing on it is not a stem */
  d.tracks[1].muted = 1;

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.overwrite = 1;
  CHECK_EQ_INT(aud_export_stems(&d, &opts, &written, &why), 0);
  CHECK_EQ_INT((int)written, 1);

  /* numbered by the lane, so the gap says which one was left out */
  CHECK(access(stem_path(0, d.tracks[0].name), F_OK) == 0);
  CHECK(access(stem_path(1, d.tracks[1].name), F_OK) != 0);
  CHECK(access(stem_path(2, d.tracks[2].name), F_OK) != 0);
  remove(stem_path(0, d.tracks[0].name));

  /* a project the mix would hear nothing from is an error, not an empty folder */
  d.tracks[0].muted = 1;
  CHECK_EQ_INT(aud_export_stems(&d, &opts, &written, &why), -1);
  CHECK(why != NULL);
  CHECK_EQ_INT((int)written, 0);

  aud_doc_free(&d);
}

TEST(stems_that_refuse_leave_none_behind)
{
  aud_doc d;
  aud_export_options opts;
  const char *why = NULL;

  aud_doc_init(&d, 44100);
  flat(&d, 1, 100, 0.5f);
  flat(&d, 1, 100, 0.5f);

  /*
   * The second stem lands on a file that is already there and may not be
   * replaced, so the first one - written happily a moment before - has to go
   * back too.
   */
  {
    FILE *f = fopen(stem_path(1, d.tracks[1].name), "wb");

    CHECK(f != NULL);
    if (f != NULL)
    {
      fputc('x', f);
      fclose(f);
    }
  }

  aud_export_defaults(&opts);
  opts.path = g_path;
  opts.overwrite = 0;
  CHECK_EQ_INT(aud_export_stems(&d, &opts, NULL, &why), -1);
  CHECK(why != NULL);
  CHECK(access(stem_path(0, d.tracks[0].name), F_OK) != 0);

  remove(stem_path(1, d.tracks[1].name));
  aud_doc_free(&d);
}

TEST(a_take_recorded_into_a_track_reads_back_frame_for_frame)
{
  aud_track t;
  float in[4];
  float out[4];

  aud_track_init(&t, "live", 1);
  CHECK_EQ_INT(aud_track_record_begin(&t, 100, 8), 0);
  CHECK_EQ_INT(aud_track_recording(&t), 1);

  /* more than the capacity it was given, so the block has to grow */
  for (int pass = 0; pass < 10; pass++)
  {
    for (int i = 0; i < 4; i++)
    {
      in[i] = (float)(pass * 4 + i);
    }
    CHECK_EQ_INT((int)aud_track_record_push(&t, in, 4), 4);
  }

  /* and it is readable while it is still being recorded into */
  aud_track_read(&t, 100, out, 4);
  CHECK(out[0] == 0.0f && out[3] == 3.0f);
  CHECK_EQ_INT((int)aud_track_end(&t), 140);

  aud_track_record_end(&t);
  CHECK_EQ_INT(aud_track_recording(&t), 0);
  CHECK_EQ_INT(t.count, 1);

  aud_track_read(&t, 136, out, 4);
  CHECK(out[0] == 36.0f && out[3] == 39.0f);

  aud_track_free(&t);
}

TEST(recording_nothing_leaves_no_clip_behind)
{
  aud_track t;

  aud_track_init(&t, "live", 1);
  aud_track_record_begin(&t, 0, 16);
  aud_track_record_end(&t);
  CHECK_EQ_INT(t.count, 0);
  CHECK_EQ_INT((int)aud_track_end(&t), 0);

  aud_track_free(&t);
}

TEST(a_growing_block_is_summarised_as_it_grows)
{
  aud_track t;
  aud_peak p;
  float *chunk = malloc(1000 * sizeof(float));

  aud_track_init(&t, "live", 1);
  aud_track_record_begin(&t, 0, 8);

  /* enough to fill several peak buckets, so the index is exercised */
  for (int pass = 0; pass < 10; pass++)
  {
    for (int i = 0; i < 1000; i++)
    {
      chunk[i] = (pass == 5 && i == 0) ? 0.9f : 0.1f;
    }
    aud_track_record_push(&t, chunk, 1000);
  }

  /* the loud sample is found whether it is in an indexed bucket or the tail */
  aud_track_range(&t, 0, 0, 10000, &p);
  CHECK_EQ_DBL(p.max, 0.9, 1e-6);

  aud_track_range(&t, 0, 0, 100, &p);
  CHECK_EQ_DBL(p.max, 0.1, 1e-6);

  aud_track_record_end(&t);
  aud_track_range(&t, 0, 0, 10000, &p);
  CHECK_EQ_DBL(p.max, 0.9, 1e-6);

  free(chunk);
  aud_track_free(&t);
}

int main(void)
{
  snprintf(g_path, sizeof(g_path), "audiaki-mix-test-%ld.wav", (long)getpid());

  RUN(tracks_add_up);
  RUN(a_mono_track_reaches_both_sides);
  RUN(gain_scales_and_mute_silences);
  RUN(solo_silences_everything_that_is_not);
  RUN(pan_moves_a_track_without_making_it_louder);
  RUN(a_gap_and_the_space_past_the_end_are_silent);
  RUN(a_clip_gain_reaches_the_mix_the_way_a_fader_does);
  RUN(an_export_reads_back_as_what_was_mixed);
  RUN(an_export_of_the_selection_writes_only_that);
  RUN(an_export_that_refuses_leaves_no_file);
  RUN(stems_add_up_to_the_mix);
  RUN(a_stem_is_named_after_its_track);
  RUN(stems_skip_what_the_mix_cannot_hear);
  RUN(stems_that_refuse_leave_none_behind);
  RUN(a_take_recorded_into_a_track_reads_back_frame_for_frame);
  RUN(recording_nothing_leaves_no_clip_behind);
  RUN(a_growing_block_is_summarised_as_it_grows);

  remove(g_path);
  return TEST_RESULT();
}
