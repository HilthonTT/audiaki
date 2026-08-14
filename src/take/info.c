/* SPDX-License-Identifier: MIT */
#include "take/info.h"

#include "audio/format.h"
#include "audio/loudness.h"
#include "media/wav.h"
#include "util/jsonout.h"
#include "util/log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* frames decoded per read; unrelated to the noise floor window */
#define INFO_CHUNK_FRAMES 4096u

/* the window the noise floor is measured over, and how many of them to keep */
#define INFO_WINDOW_DIVISOR 20u /* rate / 20 = 50 ms */
#define INFO_MAX_WINDOWS 65536u

/* which window, once sorted, counts as "the room rather than the playing" */
#define INFO_FLOOR_PERCENTILE 10u

/*
 * The magnitude a sample has to reach to count as clipped. Signed PCM runs
 * from -1.0 to just under it, so full scale positive is one step short; float
 * has no such ceiling, and anything past 1.0 will clip when it is converted.
 */
static double clip_level(const wav_reader *r)
{
  if (r->is_float)
  {
    return 1.0;
  }
  return 1.0 - 1.0 / ldexp(1.0, (int)r->bits - 1) - 1e-9;
}

static int compare_double(const void *a, const void *b)
{
  double x = *(const double *)a;
  double y = *(const double *)b;

  if (x < y)
  {
    return -1;
  }
  return x > y ? 1 : 0;
}

int aud_info_analyse(const char *path, aud_info_report *out)
{
  wav_reader r;
  float *chunk = NULL;
  double *windows = NULL;
  aud_loudness *loud = NULL;
  double sum[AUD_INFO_MAX_CHANNELS];
  double sumsq[AUD_INFO_MAX_CHANNELS];
  double peak[AUD_INFO_MAX_CHANNELS];
  double threshold;
  double window_sumsq = 0.0;
  size_t window_frames;
  size_t window_count = 0;
  size_t window_cap;
  size_t window_have = 0;
  uint64_t frames = 0;
  uint64_t header_frames;
  uint64_t clipped = 0;
  unsigned channels;
  int rc = -1;

  if (path == NULL || out == NULL)
  {
    errno = EINVAL;
    return -1;
  }

  memset(out, 0, sizeof(*out));
  /* zero is a real loudness, so the fields cannot be left at what memset made
   * them; this is how "nothing measured" is spelled */
  aud_loudness_read(NULL, &out->loudness);

  if (wav_read_open(&r, path) != 0)
  {
    if (r.error != NULL && errno != 0)
    {
      aud_error("cannot read %s: %s (%s)", path, r.error, strerror(errno));
    }
    else
    {
      aud_error("cannot read %s: %s", path,
                r.error != NULL ? r.error : "unrecognised file");
    }
    return -1;
  }

  channels = r.channels;
  out->meta = r.meta;
  /* the reader shrinks r.frames the moment a file turns out to be short, so
   * what the data chunk claimed has to be taken down before reading starts */
  header_frames = r.frames;
  memset(sum, 0, sizeof(sum));
  memset(sumsq, 0, sizeof(sumsq));
  memset(peak, 0, sizeof(peak));
  threshold = clip_level(&r);

  /*
   * Windows are 50 ms until a take is long enough that keeping one array entry
   * each would cost real memory; past that they stretch instead. The floor is
   * a percentile either way, so a coarser window only smooths it slightly.
   */
  window_frames = r.rate / INFO_WINDOW_DIVISOR;
  if (window_frames == 0)
  {
    window_frames = 1;
  }
  if (r.frames / window_frames > INFO_MAX_WINDOWS)
  {
    window_frames = (size_t)(r.frames / INFO_MAX_WINDOWS) + 1;
  }
  window_cap = (size_t)(r.frames / window_frames) + 2;

  chunk = malloc(INFO_CHUNK_FRAMES * channels * sizeof(*chunk));
  windows = malloc(window_cap * sizeof(*windows));
  if (chunk == NULL || windows == NULL)
  {
    aud_perror("cannot measure %s", path);
    goto out;
  }

  /*
   * A rate BS.1770 cannot be derived at is not a failure: everything else about
   * the file is still worth reporting, and the loudness fields already say they
   * hold nothing. Anything else that goes wrong here is out of memory, which is.
   */
  if (aud_loudness_supported(r.rate, channels))
  {
    loud = aud_loudness_create(r.rate, channels);
    if (loud == NULL)
    {
      aud_perror("cannot measure %s", path);
      goto out;
    }
  }

  for (;;)
  {
    long got = wav_read_frames(&r, chunk, INFO_CHUNK_FRAMES);

    if (got < 0)
    {
      aud_error("cannot read %s: %s", path, r.error != NULL ? r.error : "read error");
      goto out;
    }
    if (got == 0)
    {
      break;
    }

    if (loud != NULL && aud_loudness_feed(loud, chunk, (size_t)got) != 0)
    {
      aud_perror("cannot measure %s", path);
      goto out;
    }

    for (long f = 0; f < got; f++)
    {
      for (unsigned c = 0; c < channels; c++)
      {
        double v = (double)chunk[(size_t)f * channels + c];
        double magnitude = fabs(v);

        sum[c] += v;
        sumsq[c] += v * v;
        if (magnitude > peak[c])
        {
          peak[c] = magnitude;
        }
        if (magnitude >= threshold)
        {
          clipped++;
        }
        window_sumsq += v * v;
      }

      if (++window_have == window_frames)
      {
        if (window_count < window_cap)
        {
          windows[window_count++] =
              sqrt(window_sumsq / (double)(window_frames * channels));
        }
        window_sumsq = 0.0;
        window_have = 0;
      }
    }

    frames += (uint64_t)got;
  }

  /* a trailing partial window still describes real audio, so keep it */
  if (window_have > 0 && window_count < window_cap)
  {
    windows[window_count++] = sqrt(window_sumsq / (double)(window_have * channels));
  }

  out->rate = r.rate;
  out->channels = r.channels;
  out->bits = r.bits;
  out->is_float = r.is_float;
  out->frames = frames;
  out->header_frames = header_frames > frames ? header_frames : frames;
  out->duration = r.rate > 0 ? (double)frames / (double)r.rate : 0.0;
  out->clipped = clipped;
  out->samples = frames * channels;

  for (unsigned c = 0; c < channels; c++)
  {
    out->channel_peak[c] = peak[c];
    out->channel_rms[c] = frames > 0 ? sqrt(sumsq[c] / (double)frames) : 0.0;
    out->channel_dc[c] = frames > 0 ? sum[c] / (double)frames : 0.0;
    if (peak[c] > out->peak)
    {
      out->peak = peak[c];
    }
    out->rms += sumsq[c];
  }
  out->rms = out->samples > 0 ? sqrt(out->rms / (double)out->samples) : 0.0;

  if (window_count > 0)
  {
    qsort(windows, window_count, sizeof(*windows), compare_double);
    out->noise_floor = windows[window_count * INFO_FLOOR_PERCENTILE / 100u];
  }

  if (loud != NULL)
  {
    aud_loudness_read(loud, &out->loudness);
  }

  rc = 0;

out:
  aud_loudness_destroy(loud);
  free(windows);
  free(chunk);
  wav_read_close(&r);
  return rc;
}

/* "12.34" under a minute, "01:12.34" over one, "1:01:12.34" over an hour. */
static void format_clock(char *dst, size_t size, double seconds)
{
  /* round to the centisecond first, so 59.999 s reads as 01:00.00 not 60.00 */
  double rounded = floor(seconds * 100.0 + 0.5) / 100.0;
  unsigned long whole = (unsigned long)rounded;
  unsigned hours = (unsigned)(whole / 3600ul);
  unsigned minutes = (unsigned)((whole / 60ul) % 60ul);
  double rest = rounded - (double)(whole - whole % 60ul);

  if (hours > 0)
  {
    snprintf(dst, size, "%u:%02u:%05.2f", hours, minutes, rest);
  }
  else if (minutes > 0)
  {
    snprintf(dst, size, "%02u:%05.2f", minutes, rest);
  }
  else
  {
    snprintf(dst, size, "%.2f", rest);
  }
}

static const char *encoding_name(const aud_info_report *r)
{
  return r->is_float ? "float" : "PCM";
}

/*
 * One loudness line, laid out like the dBFS ones above it, or the reason there
 * is no figure. Which reason matters: a take can be too short for the window
 * the measurement is defined over or too quiet to reach its gate, and those
 * send you to two different places. A bare dash sends you to neither.
 */
static void print_loudness(FILE *out, const char *label, double value, const char *units,
                           const char *absent)
{
  if (aud_loudness_measured(value))
  {
    fprintf(out, "%-12s %.1f %s\n", label, value, units);
  }
  else
  {
    fprintf(out, "%-12s %s\n", label, absent);
  }
}

void aud_info_print(FILE *out, const char *path, const aud_info_report *r)
{
  char clock[32];
  char too_slow[80];
  const char *short_take;
  const char *long_take;

  if (out == NULL || r == NULL)
  {
    return;
  }

  format_clock(clock, sizeof(clock), r->duration);

  snprintf(too_slow, sizeof(too_slow), "n/a  (not measurable below %u Hz)",
           AUD_LOUDNESS_MIN_RATE);
  short_take = r->duration < 0.4 ? "n/a  (needs 400 ms of audio)"
                                 : "n/a  (nothing above the -70 LUFS gate)";
  long_take = r->duration < 3.0 ? "n/a  (needs 3 s of audio)"
                                : "n/a  (nothing above the -70 LUFS gate)";
  if (!aud_loudness_supported(r->rate, r->channels))
  {
    short_take = too_slow;
    long_take = too_slow;
  }

  fprintf(out, "file:        %s\n", path != NULL ? path : "-");
  fprintf(out, "format:      %u bit %s\n", r->bits, encoding_name(r));
  fprintf(out, "channels:    %u\n", r->channels);
  fprintf(out, "rate:        %u Hz\n", r->rate);
  fprintf(out, "duration:    %s  (%llu frames)\n", clock, (unsigned long long)r->frames);

  if (r->header_frames > r->frames)
  {
    fprintf(out, "truncated:   header claims %llu frames, %llu are present\n",
            (unsigned long long)r->header_frames, (unsigned long long)r->frames);
  }

  fprintf(out, "peak:        %.1f dBFS\n", aud_format_dbfs(r->peak));
  fprintf(out, "true peak:   %.1f dBTP\n", aud_format_dbfs(r->loudness.true_peak));
  fprintf(out, "rms:         %.1f dBFS\n", aud_format_dbfs(r->rms));
  print_loudness(out, "loudness:", r->loudness.integrated, "LUFS", short_take);
  print_loudness(out, "range:", r->loudness.range, "LU", long_take);
  print_loudness(out, "momentary:", r->loudness.momentary_max, "LUFS", short_take);
  print_loudness(out, "short-term:", r->loudness.short_max, "LUFS", long_take);
  fprintf(out, "noise floor: %.1f dBFS\n", aud_format_dbfs(r->noise_floor));
  fprintf(out, "clipped:     %llu sample(s)\n", (unsigned long long)r->clipped);

  if (r->channels > 1)
  {
    for (unsigned c = 0; c < r->channels; c++)
    {
      fprintf(out, "%s ch %u: peak %6.1f dBFS  rms %6.1f dBFS  dc %+.5f\n",
              c == 0 ? "channels:   " : "            ", c + 1,
              aud_format_dbfs(r->channel_peak[c]), aud_format_dbfs(r->channel_rms[c]),
              r->channel_dc[c]);
    }
  }
  else if (r->channels == 1)
  {
    fprintf(out, "dc offset:   %+.5f\n", r->channel_dc[0]);
  }

  /*
   * Last, and only what is there. Most WAV files carry none of this, and a
   * column of empty fields would say less than their absence does.
   */
  if (r->meta.recorded[0] != '\0')
  {
    fprintf(out, "recorded:    %s\n", r->meta.recorded);
  }
  if (r->meta.device[0] != '\0')
  {
    fprintf(out, "device:      %s\n", r->meta.device);
  }
  if (r->meta.software[0] != '\0')
  {
    fprintf(out, "software:    %s\n", r->meta.software);
  }
  if (r->meta.tempo[0] != '\0')
  {
    fprintf(out, "metronome:   %s\n", r->meta.tempo);
  }
  if (r->meta.note[0] != '\0')
  {
    fprintf(out, "note:        %s\n", r->meta.note);
  }
}

/* -- one line per take ----------------------------------------------------- */

/* room for a name, without letting one long path push the numbers off screen */
#define INFO_NAME_MIN 12u
#define INFO_NAME_MAX 40u

unsigned aud_info_row_width(unsigned longest_name)
{
  if (longest_name < INFO_NAME_MIN)
  {
    return INFO_NAME_MIN;
  }
  return longest_name > INFO_NAME_MAX ? INFO_NAME_MAX : longest_name;
}

/*
 * Long names lose their middle rather than their end: takes in a session differ
 * in the last few characters, and a column of identical prefixes would not say
 * which take is which.
 */
static void fit_name(char *dst, size_t size, const char *path, unsigned width)
{
  size_t len = path != NULL ? strlen(path) : 1u;
  size_t keep;

  if (path == NULL)
  {
    path = "-";
  }
  if (len <= width || width + 1u > size)
  {
    snprintf(dst, size, "%s", path);
    return;
  }

  keep = width - 3u; /* the ellipsis */
  snprintf(dst, size, "...%s", path + (len - keep));
}

void aud_info_print_row_header(FILE *out, unsigned width)
{
  if (out == NULL)
  {
    return;
  }
  fprintf(out, "%-*s  %10s %8s %8s %8s %9s\n", (int)width, "FILE", "DURATION", "PEAK",
          "RMS", "LUFS", "CLIPPED");
}

void aud_info_print_row(FILE *out, const char *path, const aud_info_report *r,
                        unsigned width)
{
  char name[INFO_NAME_MAX + 8u];
  char clock[32];
  char lufs[16];

  if (out == NULL || r == NULL)
  {
    return;
  }

  fit_name(name, sizeof(name), path, width);
  format_clock(clock, sizeof(clock), r->duration);

  /*
   * The column the question is usually settled by: peak says which take has
   * headroom left and this says which one is actually louder, which are not the
   * same take. A row is a comparison, so a take with no figure gets a mark
   * rather than a blank - the column has to line up either way.
   */
  if (aud_loudness_measured(r->loudness.integrated))
  {
    snprintf(lufs, sizeof(lufs), "%.1f", r->loudness.integrated);
  }
  else
  {
    snprintf(lufs, sizeof(lufs), "%s", "n/a");
  }

  fprintf(out, "%-*s  %10s %8.1f %8.1f %8s %9llu%s\n", (int)width, name, clock,
          aud_format_dbfs(r->peak), aud_format_dbfs(r->rms), lufs,
          (unsigned long long)r->clipped, r->clipped > 0 ? "  CLIP" : "");
}

/* An absent metadata field is JSON null rather than "", which aud_json_string
 * writes for a NULL pointer. */
static const char *or_null(const char *s)
{
  return *s != '\0' ? s : NULL;
}

/*
 * A loudness, or JSON null when there is not one. aud_json_number() already
 * writes null for a NaN, so the absence travels through the one path the
 * numbers do rather than needing the printer to branch around it.
 */
static void json_loudness(FILE *out, double value)
{
  aud_json_number(out, aud_loudness_measured(value) ? value : NAN, 2);
}

void aud_info_print_json(FILE *out, const char *path, const aud_info_report *r)
{
  if (out == NULL || r == NULL)
  {
    return;
  }

  fputs("{\n  \"file\": ", out);
  aud_json_string(out, path);
  fprintf(out, ",\n  \"rate\": %u", r->rate);
  fprintf(out, ",\n  \"channels\": %u", r->channels);
  fprintf(out, ",\n  \"bits\": %u", r->bits);
  fputs(",\n  \"encoding\": ", out);
  aud_json_string(out, r->is_float ? "float" : "pcm");
  fprintf(out, ",\n  \"frames\": %llu", (unsigned long long)r->frames);
  fprintf(out, ",\n  \"header_frames\": %llu", (unsigned long long)r->header_frames);
  fprintf(out, ",\n  \"truncated\": %s", r->header_frames > r->frames ? "true" : "false");
  fputs(",\n  \"duration\": ", out);
  aud_json_number(out, r->duration, 3);
  fputs(",\n  \"peak_dbfs\": ", out);
  aud_json_number(out, aud_format_dbfs(r->peak), 2);
  fputs(",\n  \"true_peak_dbtp\": ", out);
  aud_json_number(out, aud_format_dbfs(r->loudness.true_peak), 2);
  fputs(",\n  \"rms_dbfs\": ", out);
  aud_json_number(out, aud_format_dbfs(r->rms), 2);
  fputs(",\n  \"noise_floor_dbfs\": ", out);
  aud_json_number(out, aud_format_dbfs(r->noise_floor), 2);
  fprintf(out, ",\n  \"clipped_samples\": %llu", (unsigned long long)r->clipped);

  /*
   * Always present, like `metadata` below and for the same reason: a script can
   * read .loudness.integrated_lufs without first asking whether the key is
   * there. The fields inside are null when the take had nothing to measure.
   */
  fputs(",\n  \"loudness\": {", out);
  fputs("\n    \"integrated_lufs\": ", out);
  json_loudness(out, r->loudness.integrated);
  fputs(",\n    \"range_lu\": ", out);
  json_loudness(out, r->loudness.range);
  fputs(",\n    \"momentary_max_lufs\": ", out);
  json_loudness(out, r->loudness.momentary_max);
  fputs(",\n    \"short_term_max_lufs\": ", out);
  json_loudness(out, r->loudness.short_max);
  fputs("\n  }", out);

  fputs(",\n  \"per_channel\": [", out);
  for (unsigned c = 0; c < r->channels; c++)
  {
    fputs(c == 0 ? "\n    {" : ",\n    {", out);
    fputs("\"peak_dbfs\": ", out);
    aud_json_number(out, aud_format_dbfs(r->channel_peak[c]), 2);
    fputs(", \"rms_dbfs\": ", out);
    aud_json_number(out, aud_format_dbfs(r->channel_rms[c]), 2);
    fputs(", \"dc_offset\": ", out);
    aud_json_number(out, r->channel_dc[c], 6);
    fputc('}', out);
  }
  fputs(r->channels > 0 ? "\n  ]" : "]", out);

  /*
   * Always present, so a script can read .metadata.note without checking
   * whether the key exists; the fields inside are null when the file said
   * nothing, which is what "unknown" looks like in JSON.
   */
  fputs(",\n  \"metadata\": {", out);
  fputs("\n    \"recorded\": ", out);
  aud_json_string(out, or_null(r->meta.recorded));
  fputs(",\n    \"device\": ", out);
  aud_json_string(out, or_null(r->meta.device));
  fputs(",\n    \"software\": ", out);
  aud_json_string(out, or_null(r->meta.software));
  fputs(",\n    \"note\": ", out);
  aud_json_string(out, or_null(r->meta.note));
  fputs(",\n    \"metronome\": ", out);
  aud_json_string(out, or_null(r->meta.tempo));
  fputs(",\n    \"coding_history\": ", out);
  aud_json_string(out, or_null(r->meta.coding_history));
  fputs("\n  }\n}\n", out);
}
