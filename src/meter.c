/* SPDX-License-Identifier: MIT */
#include "meter.h"

#include "format.h"
#include "spectrum.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define METER_MIN_WIDTH 10
#define METER_MAX_WIDTH 60
#define METER_DEFAULT_WIDTH 30
#define METER_RANGE_DB 60.0 /* bottom of the scale, in dBFS */

/*
 * Columns the spectrum line needs for everything that is not a bar:
 * " 00:00 " is 7, and "  -12.3 dBFS  xruns:0    CLIP" is 29. One spare on top,
 * so a full width line cannot wrap and strand the cursor on the next row.
 */
#define METER_READOUT_COLS 37

static int terminal_cols(void)
{
  struct winsize ws;

  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (int)ws.ws_col;
  return METER_DEFAULT_WIDTH + METER_READOUT_COLS;
}

static int terminal_width(void)
{
  /* leave room for the timestamp, dB readout and xrun counter */
  int width = terminal_cols() - 40;

  if (width < METER_MIN_WIDTH)
    return METER_MIN_WIDTH;
  if (width > METER_MAX_WIDTH)
    return METER_MAX_WIDTH;
  return width;
}

/*
 * Whether to draw with block characters. Checked from the environment rather
 * than nl_langinfo() because that would need a setlocale() call, which would
 * change how the rest of the program formats numbers.
 */
static int terminal_is_utf8(void)
{
  static const char *const vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};

  for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
  {
    const char *value = getenv(vars[i]);

    if (value == NULL || *value == '\0')
      continue;
    /* the first variable that is set decides, as it does for locale lookup */
    return strstr(value, "UTF-8") != NULL || strstr(value, "utf8") != NULL ||
           strstr(value, "UTF8") != NULL || strstr(value, "utf-8") != NULL;
  }
  return 0;
}

void meter_init(aud_meter *m, int want)
{
  memset(m, 0, sizeof(*m));
  m->enabled = want && isatty(STDERR_FILENO);
  m->width = m->enabled ? terminal_width() : METER_DEFAULT_WIDTH;
  m->unicode = m->enabled ? terminal_is_utf8() : 0;
}

size_t meter_fit_bands(const aud_meter *m)
{
  int cols;

  if (!m->enabled)
    return 0;

  cols = terminal_cols() - METER_READOUT_COLS;
  if (cols < (int)AUD_SPECTRUM_MIN_BANDS)
    cols = (int)AUD_SPECTRUM_MIN_BANDS;
  if (cols > (int)AUD_SPECTRUM_MAX_BANDS)
    cols = (int)AUD_SPECTRUM_MAX_BANDS;
  return (size_t)cols;
}

/*
 * Peak hold and clip detection run whether or not anything is drawn: the
 * summary after a recording reports clipping even under --no-meter.
 */
static void track_peak(aud_meter *m, double peak)
{
  if (peak > m->hold_peak)
    m->hold_peak = peak;
  if (peak >= AUD_CLIP_THRESHOLD)
    m->clipped = 1;
}

void meter_draw(aud_meter *m, double peak, double seconds, unsigned xruns)
{
  char bar[METER_MAX_WIDTH + 1];
  double db;
  int filled;
  int hold_pos;
  int minutes;

  track_peak(m, peak);

  if (!m->enabled)
    return;

  db = aud_format_dbfs(peak);
  filled = (int)((db + METER_RANGE_DB) / METER_RANGE_DB * m->width);
  if (filled < 0)
    filled = 0;
  if (filled > m->width)
    filled = m->width;

  hold_pos =
      (int)((aud_format_dbfs(m->hold_peak) + METER_RANGE_DB) / METER_RANGE_DB * m->width);
  if (hold_pos < 0)
    hold_pos = 0;
  if (hold_pos >= m->width)
    hold_pos = m->width - 1;

  memset(bar, ' ', (size_t)m->width);
  for (int i = 0; i < filled; i++)
    bar[i] = '#';
  if (hold_pos >= filled)
    bar[hold_pos] = '|'; /* peak hold marker */
  bar[m->width] = '\0';

  minutes = (int)seconds / 60;
  fprintf(stderr, "\r %02d:%02d [%s] %6.1f dBFS  xruns:%-4u%s", minutes,
          (int)seconds % 60, bar, db, xruns, m->clipped ? " CLIP" : "");
  fflush(stderr);
  m->line_dirty = 1;
}

/* 8 shades of vertical block, U+2581 to U+2588; index 0 is a blank column. */
#define METER_BLOCK_LEVELS 8

static const char *const meter_ascii_ramp = " .:-=+*#@";

void meter_draw_spectrum(aud_meter *m, const float *bands, size_t n, double peak,
                         double seconds, unsigned xruns)
{
  /* worst case: every band a 3 byte block character */
  char bar[AUD_SPECTRUM_MAX_BANDS * 3 + 1];
  size_t at = 0;
  double db;
  int minutes;

  track_peak(m, peak);

  if (!m->enabled || bands == NULL || n == 0)
    return;

  if (n > AUD_SPECTRUM_MAX_BANDS)
    n = AUD_SPECTRUM_MAX_BANDS;

  for (size_t b = 0; b < n; b++)
  {
    double v = (double)bands[b];
    int level;

    if (v < 0.0)
      v = 0.0;
    if (v > 1.0)
      v = 1.0;
    level = (int)(v * (double)METER_BLOCK_LEVELS + 0.5);

    if (!m->unicode)
    {
      bar[at++] = meter_ascii_ramp[level];
      continue;
    }

    if (level == 0)
    {
      bar[at++] = ' ';
      continue;
    }
    /* U+2581 + (level - 1) encodes to E2 96 (80 + level) */
    bar[at++] = (char)0xE2;
    bar[at++] = (char)0x96;
    bar[at++] = (char)(0x80 + level);
  }
  bar[at] = '\0';

  db = aud_format_dbfs(peak);
  minutes = (int)seconds / 60;

  fprintf(stderr, "\r %02d:%02d %s %6.1f dBFS  xruns:%-4u%s", minutes, (int)seconds % 60,
          bar, db, xruns, m->clipped ? " CLIP" : "");
  fflush(stderr);
  m->line_dirty = 1;
}

/*
 * Half a semitone either side of the note. Wider would waste the scale on
 * distances nobody tunes by, and the note name has already changed by then
 * anyway - past 50 cents the needle would be pointing at the wrong note.
 */
#define METER_TUNER_RANGE_CENTS 50.0

/* Columns the tuner line spends on everything that is not the scale. */
#define METER_TUNER_READOUT_COLS 4

void meter_draw_tuner(aud_meter *m, const aud_tuner_reading *reading)
{
  char bar[METER_MAX_WIDTH + 1];
  char label[AUD_TUNER_LABEL_MAX];
  char cents[16];
  char freq[16];
  int width = m->width - METER_TUNER_READOUT_COLS;
  int centre;

  if (!m->enabled || reading == NULL)
    return;

  if (width > METER_MAX_WIDTH)
    width = METER_MAX_WIDTH;
  /* an even scale has no middle column for the note itself to sit on */
  if ((width % 2) == 0)
    width--;
  if (width < 9)
    width = 9;
  centre = width / 2;

  memset(bar, '.', (size_t)width);
  bar[centre] = '|';
  bar[width] = '\0';

  aud_tuner_note_label(reading, label, sizeof(label));

  if (reading->voiced)
  {
    double offset = reading->cents / METER_TUNER_RANGE_CENTS;
    int needle;

    if (offset < -1.0)
      offset = -1.0;
    if (offset > 1.0)
      offset = 1.0;

    needle = centre + (int)lround(offset * (double)centre);
    if (needle < 0)
      needle = 0;
    if (needle >= width)
      needle = width - 1;
    bar[needle] = '#';

    if (fabs(reading->cents) <= AUD_TUNER_IN_TUNE_CENTS)
      snprintf(cents, sizeof(cents), "in tune");
    else
      snprintf(cents, sizeof(cents), "%+.0f cents", reading->cents);

    snprintf(freq, sizeof(freq), "%.1f Hz", reading->frequency);
  }
  else
  {
    /*
     * Still a full line, with the level on it. A tuner that goes blank when
     * nothing is being played looks the same as one that is not listening.
     */
    snprintf(cents, sizeof(cents), "listening");
    snprintf(freq, sizeof(freq), "--");
  }

  fprintf(stderr, "\r %-4s[%s]  %-10s %9s  %6.1f dBFS", label, bar, cents, freq,
          reading->level_db);
  fflush(stderr);
  m->line_dirty = 1;
}

void meter_clear(aud_meter *m)
{
  int cols;

  if (!m->line_dirty)
    return;

  /*
   * Overwrite the line rather than relying on an erase escape sequence. One
   * column short of the terminal width, so the blanks cannot wrap onto a
   * second line and leave the cursor there.
   */
  cols = terminal_cols() - 1;
  if (cols < METER_DEFAULT_WIDTH)
    cols = METER_DEFAULT_WIDTH;
  fprintf(stderr, "\r%*s\r", cols, "");
  fflush(stderr);
  m->line_dirty = 0;
}

int meter_clipped(const aud_meter *m)
{
  return m->clipped;
}
