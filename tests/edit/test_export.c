/* SPDX-License-Identifier: MIT */
/*
 * The export's own decisions, which are the ones that can be made without an
 * encoder: which format a name asks for, what a set of stems is called, and
 * what is refused before anything is opened.
 *
 * Writing FLAC, Opus or MP3 is ffmpeg's half of the job and is not asserted
 * here - CI has no ffmpeg, and a test that needed one would be a test that
 * usually did not run. What is asserted is everything that decides what ffmpeg
 * would be asked for, plus the WAV path end to end, which needs nobody.
 */
#include "test_util.h"

#include "edit/export.h"

#include "edit/load.h"
#include "media/wav.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DIR "audiaki-export-test"
#define TEST_RATE 8000u

static void in_dir(char *dst, size_t size, const char *name)
{
  snprintf(dst, size, "%s/%s", TEST_DIR, name);
}

/* A WAV of `frames` frames, for a project to be built over. */
static void write_wav(const char *path, unsigned channels, size_t frames)
{
  wav_writer w;
  int16_t *pcm = malloc(frames * channels * sizeof(*pcm));

  for (size_t f = 0; f < frames; f++)
  {
    for (unsigned c = 0; c < channels; c++)
    {
      pcm[f * channels + c] = (int16_t)((f % 100u) * 100u + c);
    }
  }

  CHECK_EQ_INT(wav_open(&w, path, TEST_RATE, (uint16_t)channels, 16, 1), 0);
  CHECK_EQ_INT(wav_write(&w, pcm, frames * channels * sizeof(*pcm)), 0);
  CHECK_EQ_INT(wav_close(&w), 0);
  free(pcm);
}

/* Two lanes over two takes, which is enough to be a mix and a set of stems. */
static void build(aud_doc *d)
{
  char one[256];
  char two[256];

  in_dir(one, sizeof(one), "take-001.wav");
  in_dir(two, sizeof(two), "take-002.wav");

  aud_doc_init(d, 0);
  CHECK_EQ_INT(aud_edit_load_wav(d, one, NULL), 0);
  CHECK_EQ_INT(aud_edit_load_wav(d, two, NULL), 1);
  snprintf(d->tracks[0].name, sizeof(d->tracks[0].name), "Rhythm");
  snprintf(d->tracks[1].name, sizeof(d->tracks[1].name), "Lead");
}

/* -- which format a name asks for ------------------------------------------- */

TEST(the_extension_says_what_to_write)
{
  CHECK_EQ_INT(aud_export_format_of("mix.wav"), AUD_EXPORT_WAV);
  CHECK_EQ_INT(aud_export_format_of("mix.flac"), AUD_EXPORT_FLAC);
  CHECK_EQ_INT(aud_export_format_of("mix.opus"), AUD_EXPORT_OPUS);
  CHECK_EQ_INT(aud_export_format_of("mix.mp3"), AUD_EXPORT_MP3);

  /* however it was typed */
  CHECK_EQ_INT(aud_export_format_of("/a/b/MIX.FLAC"), AUD_EXPORT_FLAC);
  CHECK_EQ_INT(aud_export_format_of("Mix.Mp3"), AUD_EXPORT_MP3);

  /* a name with none is what an export has always been */
  CHECK_EQ_INT(aud_export_format_of("mix"), AUD_EXPORT_WAV);
  CHECK_EQ_INT(aud_export_format_of("/takes/session/mix"), AUD_EXPORT_WAV);

  /* a dot in a folder is not an extension, and a leading one is a hidden file */
  CHECK_EQ_INT(aud_export_format_of("/a.dir/mix"), AUD_EXPORT_WAV);
  CHECK_EQ_INT(aud_export_format_of(".wav"), AUD_EXPORT_WAV);

  /* and everything else is refused rather than handed to ffmpeg to guess at */
  CHECK_EQ_INT(aud_export_format_of("mix.aiff"), AUD_EXPORT_UNKNOWN);
  CHECK_EQ_INT(aud_export_format_of("mix.ogg"), AUD_EXPORT_UNKNOWN);
  CHECK_EQ_INT(aud_export_format_of("mix.wav.bak"), AUD_EXPORT_UNKNOWN);
  CHECK_EQ_INT(aud_export_format_of(NULL), AUD_EXPORT_UNKNOWN);
}

TEST(only_wav_is_written_without_ffmpeg)
{
  CHECK(!aud_export_format_needs_ffmpeg(AUD_EXPORT_WAV));
  CHECK(aud_export_format_needs_ffmpeg(AUD_EXPORT_FLAC));
  CHECK(aud_export_format_needs_ffmpeg(AUD_EXPORT_OPUS));
  CHECK(aud_export_format_needs_ffmpeg(AUD_EXPORT_MP3));

  /* not a format is not a job for ffmpeg either; it is a job for nobody */
  CHECK(!aud_export_format_needs_ffmpeg(AUD_EXPORT_UNKNOWN));

  CHECK(strcmp(aud_export_format_name(AUD_EXPORT_FLAC), "FLAC") == 0);
  CHECK(strcmp(aud_export_format_name(AUD_EXPORT_UNKNOWN), "") == 0);
}

/* -- what a set of stems is called ------------------------------------------ */

TEST(stems_keep_the_extension_the_set_was_asked_for)
{
  char path[AUD_PATH_MAX];

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "song.wav", 0, "Rhythm"), 0);
  CHECK(strcmp(path, "song-01-Rhythm.wav") == 0);

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "song.flac", 1, "Lead"), 0);
  CHECK(strcmp(path, "song-02-Lead.flac") == 0);

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "/t/song.MP3", 2, "Bass"), 0);
  CHECK(strcmp(path, "/t/song-03-Bass.MP3") == 0);

  /* a base with no extension still gives a set of WAVs */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "song", 0, "Rhythm"), 0);
  CHECK(strcmp(path, "song-01-Rhythm.wav") == 0);

  /* a dot in a folder is not one to strip */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "/a.dir/song", 0, "X"), 0);
  CHECK(strcmp(path, "/a.dir/song-01-X.wav") == 0);
}

TEST(a_track_name_that_would_be_a_path_is_reduced_to_one_word)
{
  char path[AUD_PATH_MAX];

  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "s.flac", 0, "Gtr / DI"), 0);
  CHECK(strcmp(path, "s-01-Gtr-DI.flac") == 0);

  /* a name that leaves nothing usable gives the number on its own */
  CHECK_EQ_INT(aud_export_stem_path(path, sizeof(path), "s.opus", 4, "///"), 0);
  CHECK(strcmp(path, "s-05.opus") == 0);
}

/* -- what is refused, and refused before anything is opened ----------------- */

TEST(a_format_audiaki_does_not_write_is_refused)
{
  aud_doc d;
  aud_export_options opts;
  char path[256];
  const char *why = NULL;

  build(&d);
  in_dir(path, sizeof(path), "mix.aiff");

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1;

  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(why != NULL);
  CHECK_EQ_INT(access(path, F_OK), -1); /* and nothing was created on the way */

  aud_doc_free(&d);
}

/*
 * A depth is a thing PCM has. Asking for one alongside a format that holds no
 * samples is refused rather than ignored - the same rule --bits already follows
 * for the commands it does not apply to.
 */
TEST(a_bit_depth_is_refused_where_it_means_nothing)
{
  aud_doc d;
  aud_export_options opts;
  char path[256];
  const char *why = NULL;

  build(&d);
  in_dir(path, sizeof(path), "mix.mp3");

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1;
  opts.bits = 16u;

  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(why != NULL);
  CHECK_EQ_INT(access(path, F_OK), -1);

  /* without one it is a perfectly good request, whatever ffmpeg then makes of it */
  opts.bits = 0u;
  CHECK_EQ_INT(aud_export_format_of(opts.path), AUD_EXPORT_MP3);

  aud_doc_free(&d);
}

TEST(flac_holds_twenty_four_bits_at_the_most)
{
  aud_doc d;
  aud_export_options opts;
  char path[256];
  const char *why = NULL;

  build(&d);
  in_dir(path, sizeof(path), "mix.flac");

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1;
  opts.bits = 32u;

  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), -1);
  CHECK(why != NULL);
  CHECK_EQ_INT(access(path, F_OK), -1);

  aud_doc_free(&d);
}

/* -- and the path that needs nobody ----------------------------------------- */

TEST(a_wav_export_still_writes_a_wav)
{
  aud_doc d;
  aud_export_options opts;
  wav_reader r;
  char path[256];
  const char *why = NULL;

  build(&d);
  in_dir(path, sizeof(path), "mix.wav");

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1;

  CHECK_EQ_INT(aud_export_wav(&d, &opts, &why), 0);
  CHECK(why == NULL);

  CHECK_EQ_INT(wav_read_open(&r, path), 0);
  CHECK_EQ_INT(r.rate, TEST_RATE);
  CHECK_EQ_INT(r.bits, 24); /* no depth asked for still means 24 */
  CHECK_EQ_INT(r.frames, aud_doc_end(&d));
  wav_read_close(&r);

  remove(path);
  aud_doc_free(&d);
}

/* A name with no extension is a WAV, and is written as one rather than refused. */
TEST(a_name_without_an_extension_is_a_wav)
{
  aud_doc d;
  aud_export_options opts;
  wav_reader r;
  char path[256];

  build(&d);
  in_dir(path, sizeof(path), "bare");

  aud_export_defaults(&opts);
  opts.path = path;
  opts.overwrite = 1;

  CHECK_EQ_INT(aud_export_wav(&d, &opts, NULL), 0);
  CHECK_EQ_INT(wav_read_open(&r, path), 0);
  CHECK_EQ_INT(r.rate, TEST_RATE);
  wav_read_close(&r);

  remove(path);
  aud_doc_free(&d);
}

int main(void)
{
  char one[256];
  char two[256];
  int rc;

  CHECK_EQ_INT(aud_path_mkdirs(TEST_DIR), 0);
  in_dir(one, sizeof(one), "take-001.wav");
  in_dir(two, sizeof(two), "take-002.wav");
  write_wav(one, 1, 400);
  write_wav(two, 2, 600);

  RUN(the_extension_says_what_to_write);
  RUN(only_wav_is_written_without_ffmpeg);
  RUN(stems_keep_the_extension_the_set_was_asked_for);
  RUN(a_track_name_that_would_be_a_path_is_reduced_to_one_word);
  RUN(a_format_audiaki_does_not_write_is_refused);
  RUN(a_bit_depth_is_refused_where_it_means_nothing);
  RUN(flac_holds_twenty_four_bits_at_the_most);
  RUN(a_wav_export_still_writes_a_wav);
  RUN(a_name_without_an_extension_is_a_wav);

  rc = TEST_RESULT();

  /* the suite leaves nothing behind, so it can be run again */
  remove(one);
  remove(two);
  rmdir(TEST_DIR);

  return rc;
}
