/* SPDX-License-Identifier: MIT */
#include "meter.h"

#include "format.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define METER_MIN_WIDTH 10
#define METER_MAX_WIDTH 60
#define METER_DEFAULT_WIDTH 30
#define METER_RANGE_DB 60.0 /* bottom of the scale, in dBFS */
#define METER_CLIP_THRESHOLD 0.999

static int terminal_width(void)
{
  struct winsize ws;

  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
  {
    /* leave room for the timestamp, dB readout and xrun counter */
    int width = (int)ws.ws_col - 40;
    if (width < METER_MIN_WIDTH)
      return METER_MIN_WIDTH;
    if (width > METER_MAX_WIDTH)
      return METER_MAX_WIDTH;
    return width;
  }
  return METER_DEFAULT_WIDTH;
}

void meter_init(aud_meter *m, int want)
{
  memset(m, 0, sizeof(*m));
  m->enabled = want && isatty(STDERR_FILENO);
  m->width = m->enabled ? terminal_width() : METER_DEFAULT_WIDTH;
}

void meter_draw(aud_meter *m, double peak, double seconds, unsigned xruns)
{
  char bar[METER_MAX_WIDTH + 1];
  double db;
  int filled;
  int hold_pos;
  int minutes;

  if (peak > m->hold_peak)
    m->hold_peak = peak;
  if (peak >= METER_CLIP_THRESHOLD)
    m->clipped = 1;

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

void meter_clear(aud_meter *m)
{
  if (!m->line_dirty)
    return;

  /* overwrite the line rather than relying on an erase escape sequence */
  fprintf(stderr, "\r%*s\r", m->width + 40, "");
  fflush(stderr);
  m->line_dirty = 0;
}

int meter_clipped(const aud_meter *m)
{
  return m->clipped;
}
