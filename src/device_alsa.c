/* SPDX-License-Identifier: MIT */
/*
 * device_alsa.c - the ALSA capture backend.
 *
 * Opens the card directly through libasound. This is the backend that talks to
 * hardware with nothing in between, which is what you want on a machine with no
 * sound server, and what you fall back to when PipeWire is not there.
 *
 * One of two translation units that include <alsa/asoundlib.h>; the other is
 * monitor_alsa.c. Everything else in src/ works in terms of aud_device.
 */
#include "backend.h"
#include "device.h"
#include "jsonout.h"
#include "log.h"
#include "version.h"

/* snd_pcm_hw_params_alloca() expands to alloca(), which strict ISO mode does
 * not declare as a builtin. */
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

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

static int open_pcm(const char *name, snd_pcm_t **out)
{
  int err = snd_pcm_open(out, name, SND_PCM_STREAM_CAPTURE, 0);

  if (err < 0)
  {
    aud_error("cannot open capture device '%s': %s", name, snd_strerror(err));
    if (err == -EBUSY)
    {
      aud_info("the device is in use - PipeWire or PulseAudio may hold it "
               "exclusively; try --backend pipewire, or -D plughw:CARD=<name>,DEV=0");
    }
    else if (err == -ENOENT)
    {
      aud_info("run '" AUDIAKI_NAME " --list' to see the available capture devices");
    }
    return -1;
  }
  return 0;
}

static aud_format select_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                aud_format wanted)
{
  if (wanted != AUD_FORMAT_UNKNOWN)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, to_alsa_format(wanted)) == 0)
    {
      return wanted;
    }
    aud_error("device does not support format %s", aud_format_name(wanted));
    return AUD_FORMAT_UNKNOWN;
  }

  for (size_t i = 0; i < sizeof(candidate_formats) / sizeof(candidate_formats[0]); i++)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, to_alsa_format(candidate_formats[i])) == 0)
    {
      return candidate_formats[i];
    }
  }

  aud_error("no supported capture format found (run --probe to see what the "
            "device offers)");
  return AUD_FORMAT_UNKNOWN;
}

static void alsa_close(aud_device *dev)
{
  if (dev->handle == NULL)
  {
    return;
  }

  snd_pcm_close((snd_pcm_t *)dev->handle);
  dev->handle = NULL;
}

static int alsa_open_capture(aud_device *dev, const aud_device_config *cfg)
{
  snd_pcm_hw_params_t *hw = NULL;
  snd_pcm_uframes_t period;
  snd_pcm_uframes_t buffer;
  snd_pcm_t *pcm = NULL;
  unsigned rate;
  int dir = 0;
  int err;

  if (open_pcm(cfg->name, &pcm) != 0)
  {
    return -1;
  }
  dev->handle = pcm;

  snd_pcm_hw_params_alloca(&hw);

  if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
  {
    aud_error("no configurations available: %s", snd_strerror(err));
    goto fail;
  }

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
  {
    aud_error("device does not support interleaved access: %s", snd_strerror(err));
    goto fail;
  }

  dev->format = select_format(pcm, hw, cfg->format);
  if (dev->format == AUD_FORMAT_UNKNOWN)
  {
    goto fail;
  }

  if ((err = snd_pcm_hw_params_set_format(pcm, hw, to_alsa_format(dev->format))) < 0)
  {
    aud_error("cannot set format %s: %s", aud_format_name(dev->format),
              snd_strerror(err));
    goto fail;
  }

  if ((err = snd_pcm_hw_params_set_channels(pcm, hw, cfg->channels)) < 0)
  {
    unsigned cmin = 0;
    unsigned cmax = 0;
    aud_error("cannot set %u channel(s): %s", cfg->channels, snd_strerror(err));
    snd_pcm_hw_params_any(pcm, hw);
    if (snd_pcm_hw_params_get_channels_min(hw, &cmin) == 0 &&
        snd_pcm_hw_params_get_channels_max(hw, &cmax) == 0)
    {
      aud_info("device supports %u..%u channels", cmin, cmax);
    }
    goto fail;
  }

  rate = cfg->rate;
  if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0)
  {
    aud_error("cannot set rate %u Hz: %s", cfg->rate, snd_strerror(err));
    goto fail;
  }
  if (rate != cfg->rate)
  {
    aud_warn("requested %u Hz, device negotiated %u Hz", cfg->rate, rate);
  }

  period = cfg->period_frames;
  dir = 0;
  if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0)
  {
    aud_error("cannot set period size: %s", snd_strerror(err));
    goto fail;
  }

  buffer = period * cfg->periods;
  if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0)
  {
    aud_error("cannot set buffer size: %s", snd_strerror(err));
    goto fail;
  }

  if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
  {
    aud_error("cannot apply hardware parameters: %s", snd_strerror(err));
    goto fail;
  }

  /* re-read: the driver may have rounded anything we asked for */
  snd_pcm_hw_params_get_period_size(hw, &period, &dir);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);

  if ((err = snd_pcm_prepare(pcm)) < 0)
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
  alsa_close(dev);
  return -1;
}

static long alsa_read(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns)
{
  snd_pcm_t *pcm = (snd_pcm_t *)dev->handle;
  snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf, (snd_pcm_uframes_t)frames);

  if (got >= 0)
  {
    return (long)got;
  }

  if (got == -EPIPE && xruns != NULL)
  {
    (*xruns)++;
  }

  /* snd_pcm_recover handles EPIPE, ESTRPIPE and EINTR; anything else is fatal */
  got = snd_pcm_recover(pcm, (int)got, 1 /* silent */);
  if (got < 0)
  {
    aud_error("capture failed: %s", snd_strerror((int)got));
    return -1;
  }
  return 0;
}

static void alsa_drop(aud_device *dev)
{
  if (dev->handle != NULL)
  {
    snd_pcm_drop((snd_pcm_t *)dev->handle);
  }
}

static int alsa_probe(const char *name, int json)
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
  {
    return -1;
  }

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
    fputs(",\n  \"backend\": \"alsa\"", stdout);
    fputs(",\n  \"formats\": [", stdout);
    for (int f = 0; f <= SND_PCM_FORMAT_LAST; f++)
    {
      if (snd_pcm_hw_params_test_format(pcm, hw, (snd_pcm_format_t)f) != 0)
      {
        continue;
      }
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
    printf("backend:  alsa\n");

    printf("formats: ");
    for (int f = 0; f <= SND_PCM_FORMAT_LAST; f++)
    {
      if (snd_pcm_hw_params_test_format(pcm, hw, (snd_pcm_format_t)f) == 0)
      {
        printf(" %s", snd_pcm_format_name((snd_pcm_format_t)f));
      }
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

/*
 * Grow `*list` to hold one more entry. Returns 0, or -1 leaving the existing
 * array intact for the caller to free.
 */
static int push_entry(aud_device_entry **list, int count, const char *name,
                      const char *card, const char *description)
{
  aud_device_entry *grown = realloc(*list, (size_t)(count + 1) * sizeof(**list));

  if (grown == NULL)
  {
    return -1;
  }

  *list = grown;
  snprintf(grown[count].name, sizeof(grown[count].name), "%s", name);
  snprintf(grown[count].card, sizeof(grown[count].card), "%s", card != NULL ? card : "");
  snprintf(grown[count].description, sizeof(grown[count].description), "%s",
           description != NULL ? description : "");
  return 0;
}

static int alsa_enumerate(aud_device_entry **out)
{
  snd_ctl_card_info_t *card_info = NULL;
  snd_pcm_info_t *pcm_info = NULL;
  aud_device_entry *list = NULL;
  int card = -1;
  int found = 0;

  if (out == NULL)
  {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;

  snd_ctl_card_info_alloca(&card_info);
  snd_pcm_info_alloca(&pcm_info);

  {
    int err = snd_card_next(&card);

    if (err < 0)
    {
      aud_error("cannot ask ALSA for the sound cards: %s", snd_strerror(err));
      return -1;
    }
  }

  /*
   * No cards is an answer, not a failure. Callers that watch for hardware
   * being plugged in ask again and again, and every one of those asks would
   * otherwise be an error on a machine that has nothing attached yet.
   */
  if (card < 0)
  {
    return 0;
  }

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
      {
        continue;
      } /* playback-only PCM */

      snprintf(device_name, sizeof(device_name), "hw:CARD=%s,DEV=%d",
               snd_ctl_card_info_get_id(card_info), device);

      if (push_entry(&list, found, device_name, snd_ctl_card_info_get_name(card_info),
                     snd_pcm_info_get_name(pcm_info)) != 0)
      {
        aud_error("out of memory listing devices");
        snd_ctl_close(ctl);
        free(list);
        return -1;
      }
      found++;
    }
    snd_ctl_close(ctl);

  next_card:
    if (snd_card_next(&card) < 0)
    {
      break;
    }
  }

  *out = list;
  return found;
}

/* Where the kernel puts a card's nodes the moment it registers one. */
#define WATCH_DIR "/dev/snd"

/*
 * How often to say "look again" of its own accord. This is the mechanism, not
 * the fallback: inotify on /dev/snd is not delivered everywhere - a sandbox or
 * a container can hold its own mount of devtmpfs, where nodes appear and
 * disappear exactly as they do outside and no watch on them ever fires - and a
 * device list that quietly stops updating in those is worse than one that
 * costs a re-walk. Enumerating two cards takes about 0.3 ms, which is a fifth
 * of a frame every other second.
 */
#define WATCH_SWEEP 2.0

/*
 * Seconds to let a burst of inotify events settle before reporting it, when
 * they do arrive. Plugging in one interface creates a control node and a node
 * per PCM, and enumerating between them would find a card only half there.
 * This is what makes a plugged-in device appear in well under a second rather
 * than whenever the sweep next comes round.
 */
#define WATCH_SETTLE 0.4

/*
 * One more look after a burst has been reported. Cheap insurance for hardware
 * that registers its capture PCM a little after the node that announced it,
 * which would otherwise wait for the next sweep.
 */
#define WATCH_CONFIRM 1.5

typedef struct
{
  int fd;           /* inotify descriptor, or -1 when there is none */
  double next_scan; /* monotonic deadline for the next report */
  int confirm_left; /* second looks still owed to a burst of events */
} alsa_watch;

static double watch_now(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
  {
    return 0.0;
  }
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Drain whatever inotify has queued. Returns 1 if anything was waiting. */
static int watch_drain(int fd)
{
  /* the events themselves say nothing we act on; only that something moved */
  char buf[4096];
  int saw = 0;

  while (read(fd, buf, sizeof(buf)) > 0)
  {
    saw = 1;
  }
  return saw;
}

static void *alsa_watch_create(void)
{
  alsa_watch *w = calloc(1, sizeof(*w));

  if (w == NULL)
  {
    return NULL;
  }

  w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (w->fd >= 0 &&
      inotify_add_watch(w->fd, WATCH_DIR,
                        IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM) < 0)
  {
    close(w->fd);
    w->fd = -1;
  }

  if (w->fd < 0)
  {
    aud_debug("cannot watch " WATCH_DIR " (%s); device changes will be noticed "
              "by the sweep alone",
              strerror(errno));
  }

  w->next_scan = watch_now() + WATCH_SWEEP;
  return w;
}

static void alsa_watch_destroy(void *impl)
{
  alsa_watch *w = impl;

  if (w == NULL)
  {
    return;
  }

  if (w->fd >= 0)
  {
    close(w->fd);
  }
  free(w);
}

static int alsa_watch_changed(void *impl)
{
  alsa_watch *w = impl;
  double now;

  if (w == NULL)
  {
    return 0;
  }

  now = watch_now();

  /* an event pulls the next look in, rather than being the only thing that books one */
  if (w->fd >= 0 && watch_drain(w->fd))
  {
    w->next_scan = now + WATCH_SETTLE;
    w->confirm_left = 1;
  }

  if (now < w->next_scan)
  {
    return 0;
  }

  if (w->confirm_left > 0)
  {
    w->confirm_left--;
    w->next_scan = now + WATCH_CONFIRM;
  }
  else
  {
    w->next_scan = now + WATCH_SWEEP;
  }

  return 1;
}

const aud_capture_ops aud_capture_ops_alsa = {
    .name = "alsa",
    .open_capture = alsa_open_capture,
    .close = alsa_close,
    .read = alsa_read,
    .drop = alsa_drop,
    .probe = alsa_probe,
    .enumerate = alsa_enumerate,
    .watch_create = alsa_watch_create,
    .watch_destroy = alsa_watch_destroy,
    .watch_changed = alsa_watch_changed,
};
