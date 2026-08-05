/* SPDX-License-Identifier: MIT */
#include "device.h"

#include "jsonout.h"
#include "log.h"
#include "version.h"

/* snd_pcm_hw_params_alloca() expands to alloca(), which strict ISO mode does
 * not declare as a builtin. */
#include <alloca.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Preference order when the user does not pin a format. S32_LE and S24_3LE
 * map straight into a WAV file; S24_LE needs repacking; S16_LE is the floor.
 */
static const aud_format candidate_formats[] = {
    AUD_FORMAT_S32_LE,
    AUD_FORMAT_S24_3LE,
    AUD_FORMAT_S24_LE,
    AUD_FORMAT_S16_LE,
};

static snd_pcm_format_t to_alsa_format(aud_format fmt)
{
  switch (fmt)
  {
  case AUD_FORMAT_S16_LE:
    return SND_PCM_FORMAT_S16_LE;
  case AUD_FORMAT_S24_3LE:
    return SND_PCM_FORMAT_S24_3LE;
  case AUD_FORMAT_S24_LE:
    return SND_PCM_FORMAT_S24_LE;
  case AUD_FORMAT_S32_LE:
    return SND_PCM_FORMAT_S32_LE;
  case AUD_FORMAT_UNKNOWN:
  default:
    return SND_PCM_FORMAT_UNKNOWN;
  }
}

void aud_device_config_defaults(aud_device_config *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->name = AUD_DEFAULT_DEVICE;
  cfg->rate = AUD_DEFAULT_RATE;
  cfg->channels = AUD_DEFAULT_CHANNELS;
  cfg->format = AUD_FORMAT_UNKNOWN;
  cfg->period_frames = AUD_DEFAULT_PERIOD_FRAMES;
  cfg->periods = AUD_DEFAULT_PERIODS;
}

static int open_pcm(const char *name, snd_pcm_t **out)
{
  int err = snd_pcm_open(out, name, SND_PCM_STREAM_CAPTURE, 0);

  if (err < 0)
  {
    aud_error("cannot open capture device '%s': %s", name, snd_strerror(err));
    if (err == -EBUSY)
      aud_info("the device is in use - PipeWire or PulseAudio may hold it "
               "exclusively; try -D plughw:CARD=<name>,DEV=0");
    else if (err == -ENOENT)
      aud_info("run '" AUDIAKI_NAME " --list' to see the available capture devices");
    return -1;
  }
  return 0;
}

/* Pick the first candidate format the hardware accepts. */
static aud_format select_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                aud_format wanted)
{
  if (wanted != AUD_FORMAT_UNKNOWN)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, to_alsa_format(wanted)) == 0)
      return wanted;
    aud_error("device does not support format %s", aud_format_name(wanted));
    return AUD_FORMAT_UNKNOWN;
  }

  for (size_t i = 0; i < sizeof(candidate_formats) / sizeof(candidate_formats[0]); i++)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, to_alsa_format(candidate_formats[i])) == 0)
      return candidate_formats[i];
  }

  aud_error("no supported capture format found (run --probe to see what the "
            "device offers)");
  return AUD_FORMAT_UNKNOWN;
}

int aud_device_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  snd_pcm_hw_params_t *hw = NULL;
  snd_pcm_uframes_t period;
  snd_pcm_uframes_t buffer;
  unsigned rate;
  int dir = 0;
  int err;

  memset(dev, 0, sizeof(*dev));

  if (open_pcm(cfg->name, &dev->pcm) != 0)
    return -1;

  snd_pcm_hw_params_alloca(&hw);

  if ((err = snd_pcm_hw_params_any(dev->pcm, hw)) < 0)
  {
    aud_error("no configurations available: %s", snd_strerror(err));
    goto fail;
  }

  err = snd_pcm_hw_params_set_access(dev->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
  {
    aud_error("device does not support interleaved access: %s", snd_strerror(err));
    goto fail;
  }

  dev->format = select_format(dev->pcm, hw, cfg->format);
  if (dev->format == AUD_FORMAT_UNKNOWN)
    goto fail;

  if ((err = snd_pcm_hw_params_set_format(dev->pcm, hw, to_alsa_format(dev->format))) < 0)
  {
    aud_error("cannot set format %s: %s", aud_format_name(dev->format),
              snd_strerror(err));
    goto fail;
  }

  if ((err = snd_pcm_hw_params_set_channels(dev->pcm, hw, cfg->channels)) < 0)
  {
    unsigned cmin = 0;
    unsigned cmax = 0;
    aud_error("cannot set %u channel(s): %s", cfg->channels, snd_strerror(err));
    snd_pcm_hw_params_any(dev->pcm, hw);
    if (snd_pcm_hw_params_get_channels_min(hw, &cmin) == 0 &&
        snd_pcm_hw_params_get_channels_max(hw, &cmax) == 0)
      aud_info("device supports %u..%u channels", cmin, cmax);
    goto fail;
  }

  rate = cfg->rate;
  if ((err = snd_pcm_hw_params_set_rate_near(dev->pcm, hw, &rate, &dir)) < 0)
  {
    aud_error("cannot set rate %u Hz: %s", cfg->rate, snd_strerror(err));
    goto fail;
  }
  if (rate != cfg->rate)
    aud_warn("requested %u Hz, device negotiated %u Hz", cfg->rate, rate);

  period = cfg->period_frames;
  dir = 0;
  if ((err = snd_pcm_hw_params_set_period_size_near(dev->pcm, hw, &period, &dir)) < 0)
  {
    aud_error("cannot set period size: %s", snd_strerror(err));
    goto fail;
  }

  buffer = period * cfg->periods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(dev->pcm, hw, &buffer)) < 0)
  {
    aud_error("cannot set buffer size: %s", snd_strerror(err));
    goto fail;
  }

  if ((err = snd_pcm_hw_params(dev->pcm, hw)) < 0)
  {
    aud_error("cannot apply hardware parameters: %s", snd_strerror(err));
    goto fail;
  }

  /* re-read: the driver may have rounded anything we asked for */
  snd_pcm_hw_params_get_period_size(hw, &period, &dir);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);

  if ((err = snd_pcm_prepare(dev->pcm)) < 0)
  {
    aud_error("cannot prepare stream: %s", snd_strerror(err));
    goto fail;
  }

  dev->name = cfg->name;
  dev->rate = rate;
  dev->channels = cfg->channels;
  dev->period_frames = (unsigned long)period;
  dev->buffer_frames = (unsigned long)buffer;

  aud_debug("opened %s: %s, %u Hz, %u ch, period %lu frames, buffer %lu frames",
            dev->name, aud_format_name(dev->format), dev->rate, dev->channels,
            dev->period_frames, dev->buffer_frames);
  return 0;

fail:
  aud_device_close(dev);
  return -1;
}

void aud_device_close(aud_device *dev)
{
  if (dev->pcm == NULL)
    return;

  snd_pcm_close(dev->pcm);
  dev->pcm = NULL;
}

long aud_device_read(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns)
{
  snd_pcm_sframes_t got = snd_pcm_readi(dev->pcm, buf, (snd_pcm_uframes_t)frames);

  if (got >= 0)
    return (long)got;

  if (got == -EPIPE && xruns != NULL)
    (*xruns)++;

  /* snd_pcm_recover handles EPIPE, ESTRPIPE and EINTR; anything else is fatal */
  got = snd_pcm_recover(dev->pcm, (int)got, 1 /* silent */);
  if (got < 0)
  {
    aud_error("capture failed: %s", snd_strerror((int)got));
    return -1;
  }
  return 0;
}

size_t aud_device_period_bytes(const aud_device *dev)
{
  return (size_t)dev->period_frames * dev->channels * aud_format_hw_bytes(dev->format);
}

int aud_device_probe(const char *name, int json)
{
  snd_pcm_t *pcm = NULL;
  snd_pcm_hw_params_t *hw = NULL;
  unsigned cmin = 0;
  unsigned cmax = 0;
  unsigned rmin = 0;
  unsigned rmax = 0;
  snd_pcm_uframes_t pmin = 0;
  snd_pcm_uframes_t pmax = 0;
  snd_pcm_uframes_t bmin = 0;
  snd_pcm_uframes_t bmax = 0;
  int dir = 0;
  int err;

  if (open_pcm(name, &pcm) != 0)
    return -1;

  snd_pcm_hw_params_alloca(&hw);
  if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
  {
    aud_error("no configurations available: %s", snd_strerror(err));
    snd_pcm_close(pcm);
    return -1;
  }

  snd_pcm_hw_params_get_channels_min(hw, &cmin);
  snd_pcm_hw_params_get_channels_max(hw, &cmax);
  snd_pcm_hw_params_get_rate_min(hw, &rmin, &dir);
  snd_pcm_hw_params_get_rate_max(hw, &rmax, &dir);
  snd_pcm_hw_params_get_period_size_min(hw, &pmin, &dir);
  snd_pcm_hw_params_get_period_size_max(hw, &pmax, &dir);
  snd_pcm_hw_params_get_buffer_size_min(hw, &bmin);
  snd_pcm_hw_params_get_buffer_size_max(hw, &bmax);

  if (json)
  {
    int first = 1;

    fputs("{\n  \"device\": ", stdout);
    aud_json_string(stdout, name);
    fputs(",\n  \"formats\": [", stdout);
    for (int f = 0; f <= SND_PCM_FORMAT_LAST; f++)
    {
      if (snd_pcm_hw_params_test_format(pcm, hw, (snd_pcm_format_t)f) != 0)
        continue;
      fputs(first ? "" : ", ", stdout);
      aud_json_string(stdout, snd_pcm_format_name((snd_pcm_format_t)f));
      first = 0;
    }
    printf("],\n  \"channels\": {\"min\": %u, \"max\": %u}", cmin, cmax);
    printf(",\n  \"rates\": {\"min\": %u, \"max\": %u}", rmin, rmax);
    printf(",\n  \"period_frames\": {\"min\": %lu, \"max\": %lu}", (unsigned long)pmin,
           (unsigned long)pmax);
    printf(",\n  \"buffer_frames\": {\"min\": %lu, \"max\": %lu}\n}\n",
           (unsigned long)bmin, (unsigned long)bmax);
  }
  else
  {
    printf("device:   %s\n", name);

    printf("formats: ");
    for (int f = 0; f <= SND_PCM_FORMAT_LAST; f++)
    {
      if (snd_pcm_hw_params_test_format(pcm, hw, (snd_pcm_format_t)f) == 0)
        printf(" %s", snd_pcm_format_name((snd_pcm_format_t)f));
    }
    printf("\n");

    printf("channels: %u..%u\n", cmin, cmax);
    printf("rates:    %u..%u Hz\n", rmin, rmax);
    printf("period:   %lu..%lu frames\n", (unsigned long)pmin, (unsigned long)pmax);
    printf("buffer:   %lu..%lu frames\n", (unsigned long)bmin, (unsigned long)bmax);
  }

  snd_pcm_close(pcm);
  return 0;
}

int aud_device_list(int json)
{
  snd_ctl_card_info_t *card_info = NULL;
  snd_pcm_info_t *pcm_info = NULL;
  int card = -1;
  int found = 0;

  snd_ctl_card_info_alloca(&card_info);
  snd_pcm_info_alloca(&pcm_info);

  if (snd_card_next(&card) < 0 || card < 0)
  {
    aud_error("no sound cards found");
    return -1;
  }

  if (json)
    fputs("[", stdout);
  else
    printf("%-32s %s\n", "DEVICE", "DESCRIPTION");

  while (card >= 0)
  {
    char ctl_name[32];
    snd_ctl_t *ctl = NULL;
    int device = -1;
    int err;

    snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
    if ((err = snd_ctl_open(&ctl, ctl_name, 0)) < 0)
    {
      aud_warn("cannot open control for card %d: %s", card, snd_strerror(err));
      goto next_card;
    }
    if ((err = snd_ctl_card_info(ctl, card_info)) < 0)
    {
      aud_warn("cannot read info for card %d: %s", card, snd_strerror(err));
      snd_ctl_close(ctl);
      goto next_card;
    }

    while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0)
    {
      char device_name[64];

      snd_pcm_info_set_device(pcm_info, (unsigned)device);
      snd_pcm_info_set_subdevice(pcm_info, 0);
      snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
      if (snd_ctl_pcm_info(ctl, pcm_info) < 0)
        continue; /* playback-only PCM */

      snprintf(device_name, sizeof(device_name), "hw:CARD=%s,DEV=%d",
               snd_ctl_card_info_get_id(card_info), device);

      if (json)
      {
        fputs(found == 0 ? "\n  {\"device\": " : ",\n  {\"device\": ", stdout);
        aud_json_string(stdout, device_name);
        fputs(", \"card\": ", stdout);
        aud_json_string(stdout, snd_ctl_card_info_get_name(card_info));
        fputs(", \"description\": ", stdout);
        aud_json_string(stdout, snd_pcm_info_get_name(pcm_info));
        fputc('}', stdout);
      }
      else
      {
        printf("%-32s %s: %s\n", device_name, snd_ctl_card_info_get_name(card_info),
               snd_pcm_info_get_name(pcm_info));
      }
      found++;
    }
    snd_ctl_close(ctl);

  next_card:
    if (snd_card_next(&card) < 0)
      break;
  }

  if (json)
    fputs(found > 0 ? "\n]\n" : "]\n", stdout);
  else if (found == 0)
    aud_warn("no capture devices found");
  return 0;
}
