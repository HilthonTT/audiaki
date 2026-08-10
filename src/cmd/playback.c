/* SPDX-License-Identifier: MIT */
#include "cmd/playback.h"

#include "audio/format.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What would be lost if the output went away, for the messages that say so. */
static const char *playback_what(const aud_playback *pb)
{
  if (pb->input && pb->clicking)
  {
    return "monitoring or a metronome";
  }
  return pb->input ? "monitoring" : "a metronome";
}

void aud_playback_start(aud_playback *pb, const aud_device *dev,
                        const aud_playback_config *cfg)
{
  aud_monitor_config mon_cfg;
  char bars[48]; /* room for both the bar length and the subdivision */

  memset(pb, 0, sizeof(*pb));
  pb->input = cfg->input;
  pb->gain = cfg->gain;
  bars[0] = '\0';

  if (cfg->click_bpm > 0.0)
  {
    aud_click_config click_cfg;

    /*
     * Counted in capture frames, so the grid is the capture clock rather than
     * anything the output does with it: a beat is at the frame the tempo says,
     * whatever the playback stream drops keeping up. See click.h.
     */
    aud_click_config_defaults(&click_cfg, cfg->click_bpm, dev->rate);
    click_cfg.beats_per_bar = cfg->click_beats;
    click_cfg.subdiv = cfg->click_subdiv;
    click_cfg.gain = cfg->click_gain;

    if (aud_click_init(&pb->click, &click_cfg) == 0)
    {
      size_t at = 0;

      pb->clicking = 1;
      if (cfg->click_beats > 1u)
      {
        at += (size_t)snprintf(bars, sizeof(bars), ", %u to the bar", cfg->click_beats);
      }
      if (cfg->click_subdiv > 1u && at < sizeof(bars))
      {
        snprintf(bars + at, sizeof(bars) - at, ", %u to the beat", cfg->click_subdiv);
      }
    }
    else
    {
      aud_warn("cannot run a metronome at %.4g BPM, recording without one",
               cfg->click_bpm);
    }
  }

  if (!pb->input && !pb->clicking)
  {
    return;
  }

  pb->buf = malloc((size_t)dev->period_frames * dev->channels * sizeof(*pb->buf));
  if (pb->buf == NULL)
  {
    aud_warn("cannot allocate a playback buffer, recording without %s",
             playback_what(pb));
    pb->input = 0;
    pb->clicking = 0;
    return;
  }

  aud_monitor_config_defaults(&mon_cfg, dev->rate, dev->channels);
  if (cfg->device != NULL)
  {
    mon_cfg.name = cfg->device;
  }

  pb->mon = aud_monitor_open(&mon_cfg);
  if (pb->mon == NULL)
  {
    /* the backend has already said which part of opening the output failed */
    aud_warn("recording without %s", playback_what(pb));
    free(pb->buf);
    pb->buf = NULL;
    pb->input = 0;
    pb->clicking = 0;
    return;
  }

  /*
   * A warning rather than a remark: the first thing anyone does is try this on
   * a laptop, where the default capture is the built-in microphone and the
   * default output is the speaker beside it. That is a feedback loop, and it
   * reaches full scale in a fraction of a second.
   */
  if (pb->input)
  {
    aud_warn("monitoring through %s - use headphones; a microphone played through "
             "speakers will feed back",
             mon_cfg.name);
  }

  /*
   * The click is heard and not written, which is worth saying outright: the
   * take will not have it in it, unless the room hands it back through the
   * input, which is the other reason for headphones.
   */
  if (pb->clicking)
  {
    aud_info("metronome: %.4g BPM%s through %s - heard but not recorded, so use "
             "headphones or the input will pick it up",
             cfg->click_bpm, bars, mon_cfg.name);
  }
}

void aud_playback_stop(aud_playback *pb)
{
  if (pb->mon != NULL)
  {
    pb->dropped = aud_monitor_dropped(pb->mon);
    aud_monitor_close(pb->mon);
    pb->mon = NULL;
  }

  free(pb->buf);
  pb->buf = NULL;
}

/*
 * Decodes hw_buf rather than the repacked copy for the same reason the analysis
 * does: that is what the device delivered, and the two may be the same buffer
 * anyway.
 */
void aud_playback_feed(aud_playback *pb, const unsigned char *hw_buf, size_t frames,
                       const aud_device *dev)
{
  size_t samples = frames * dev->channels;

  if (pb->mon == NULL)
  {
    return;
  }

  if (pb->input)
  {
    aud_format_to_float(pb->buf, hw_buf, frames, dev->channels, dev->format);

    /*
     * Applied here rather than passed to aud_monitor_write(), which would
     * scale the click by it as well. --monitor-gain is about how loud the
     * input is against the click, so it cannot be allowed to move both.
     */
    if (pb->gain != 1.0f)
    {
      for (size_t i = 0; i < samples; i++)
      {
        pb->buf[i] *= pb->gain;
      }
    }
  }
  else
  {
    /* nothing to hear but the beat, and aud_click_mix() adds to what it finds */
    memset(pb->buf, 0, samples * sizeof(*pb->buf));
  }

  if (pb->clicking)
  {
    aud_click_mix(&pb->click, pb->buf, frames, dev->channels);
  }

  /* the sum is clipped inside the write, which is where every path is clipped */
  if (aud_monitor_write(pb->mon, pb->buf, frames, 1.0f) != 0)
  {
    pb->dropped = aud_monitor_dropped(pb->mon);
    aud_monitor_close(pb->mon);
    pb->mon = NULL;
    aud_warn("playback stopped: the output failed (the take is still recording)");
  }
}
