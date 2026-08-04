/*
 * gtrec.c - minimal ALSA capture-to-WAV recorder
 *
 * Written for a Sonicake Smart Box (QME-20) showing up as card "Box",
 * but it will work with any ALSA capture device.
 *
 * Build:
 *     sudo apt install libasound2-dev
 *     gcc -O2 -Wall -Wextra -o gtrec gtrec.c -lasound -lm
 *
 * Use:
 *     ./gtrec --probe                 # show what the device can actually do
 *     ./gtrec take01.wav              # record until Ctrl+C
 *     ./gtrec -t 30 take02.wav        # record 30 seconds
 *     ./gtrec -D plughw:CARD=Box,DEV=0 take03.wav
 *
 * Assumes a little-endian host (x86/ARM LE). WAV is little-endian, so on a
 * big-endian machine the sample bytes would need swapping.
 */

#define _POSIX_C_SOURCE 200809L

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_DEVICE "hw:CARD=Box,DEV=0"
#define DEFAULT_RATE 44100u
#define DEFAULT_CHANNELS 2u
#define PERIOD_FRAMES 1024u
#define N_PERIODS 4u

/* RIFF sizes are 32-bit; stop well before the limit rather than corrupt. */
#define MAX_DATA_BYTES ((uint32_t)0xF0000000u)

/* ------------------------------------------------------------------ */
/* signal handling                                                     */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* format handling                                                     */
/* ------------------------------------------------------------------ */

/*
 * Preference order. S32_LE and S24_3LE both map straight into a WAV file
 * with no conversion. S24_LE is 24 valid bits inside a 4-byte container,
 * so it needs repacking to 3 bytes before writing. S16_LE is the fallback.
 */
static const snd_pcm_format_t candidate_formats[] = {
    SND_PCM_FORMAT_S32_LE,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S24_LE,
    SND_PCM_FORMAT_S16_LE,
};
static const size_t n_candidates =
    sizeof(candidate_formats) / sizeof(candidate_formats[0]);

/* bits actually written into the WAV file for a given capture format */
static unsigned wav_bits_for(snd_pcm_format_t fmt)
{
  switch (fmt)
  {
  case SND_PCM_FORMAT_S32_LE:
    return 32;
  case SND_PCM_FORMAT_S24_3LE:
    return 24;
  case SND_PCM_FORMAT_S24_LE:
    return 24; /* repacked on write */
  case SND_PCM_FORMAT_S16_LE:
    return 16;
  default:
    return 0;
  }
}

/* bytes per sample as delivered by ALSA */
static unsigned hw_bytes_for(snd_pcm_format_t fmt)
{
  switch (fmt)
  {
  case SND_PCM_FORMAT_S32_LE:
    return 4;
  case SND_PCM_FORMAT_S24_3LE:
    return 3;
  case SND_PCM_FORMAT_S24_LE:
    return 4;
  case SND_PCM_FORMAT_S16_LE:
    return 2;
  default:
    return 0;
  }
}

/* ------------------------------------------------------------------ */
/* peak meter                                                          */
/* ------------------------------------------------------------------ */

static double peak_of(const unsigned char *buf, snd_pcm_uframes_t frames,
                      unsigned channels, snd_pcm_format_t fmt)
{
  size_t n = (size_t)frames * channels;
  double peak = 0.0;

  for (size_t i = 0; i < n; i++)
  {
    double v = 0.0;

    switch (fmt)
    {
    case SND_PCM_FORMAT_S32_LE:
    {
      int32_t s;
      memcpy(&s, buf + i * 4, 4);
      v = (double)s / 2147483648.0;
      break;
    }
    case SND_PCM_FORMAT_S24_LE:
    {
      int32_t s;
      memcpy(&s, buf + i * 4, 4);
      s = (s << 8) >> 8; /* sign-extend 24 -> 32 */
      v = (double)s / 8388608.0;
      break;
    }
    case SND_PCM_FORMAT_S24_3LE:
    {
      const unsigned char *p = buf + i * 3;
      int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
      if (s & 0x00800000)
        s |= (int32_t)0xFF000000;
      v = (double)s / 8388608.0;
      break;
    }
    case SND_PCM_FORMAT_S16_LE:
    {
      int16_t s;
      memcpy(&s, buf + i * 2, 2);
      v = (double)s / 32768.0;
      break;
    }
    default:
      return 0.0;
    }

    if (v < 0.0)
      v = -v;
    if (v > peak)
      peak = v;
  }
  return peak;
}

static void draw_meter(double peak, double seconds, unsigned xruns)
{
  double db = (peak > 1e-7) ? 20.0 * log10(peak) : -99.0;
  int bars = (int)((db + 60.0) / 60.0 * 30.0); /* -60 dBFS .. 0 dBFS */
  if (bars < 0)
    bars = 0;
  if (bars > 30)
    bars = 30;

  char bar[31];
  memset(bar, ' ', 30);
  for (int i = 0; i < bars; i++)
    bar[i] = '#';
  bar[30] = '\0';

  fprintf(stderr, "\r %02d:%02d  [%s] %6.1f dBFS  xruns:%u ",
          (int)seconds / 60, (int)seconds % 60, bar, db, xruns);
  fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* WAV header                                                          */
/* ------------------------------------------------------------------ */

static void put_u32(unsigned char *p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
  p[2] = (unsigned char)((v >> 16) & 0xFF);
  p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put_u16(unsigned char *p, uint16_t v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* Canonical 44-byte PCM header. Written once with zero sizes, then
 * rewritten at the end once the real byte count is known. */
static int write_wav_header(FILE *f, uint32_t data_bytes, uint32_t rate,
                            uint16_t channels, uint16_t bits)
{
  unsigned char h[44];
  uint16_t block_align = (uint16_t)(channels * (bits / 8));
  uint32_t byte_rate = rate * block_align;

  memcpy(h + 0, "RIFF", 4);
  put_u32(h + 4, 36u + data_bytes);
  memcpy(h + 8, "WAVE", 4);

  memcpy(h + 12, "fmt ", 4);
  put_u32(h + 16, 16); /* PCM fmt chunk size */
  put_u16(h + 20, 1);  /* WAVE_FORMAT_PCM    */
  put_u16(h + 22, channels);
  put_u32(h + 24, rate);
  put_u32(h + 28, byte_rate);
  put_u16(h + 32, block_align);
  put_u16(h + 34, bits);

  memcpy(h + 36, "data", 4);
  put_u32(h + 40, data_bytes);

  if (fseek(f, 0, SEEK_SET) != 0)
    return -1;
  if (fwrite(h, 1, sizeof(h), f) != sizeof(h))
    return -1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* probe mode                                                          */
/* ------------------------------------------------------------------ */

static int probe_device(const char *device)
{
  snd_pcm_t *pcm = NULL;
  snd_pcm_hw_params_t *hw;
  int err;

  err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0)
  {
    fprintf(stderr, "cannot open %s: %s\n", device, snd_strerror(err));
    return 1;
  }

  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);

  printf("device: %s\n", device);

  printf("formats:");
  for (int f = 0; f <= SND_PCM_FORMAT_LAST; f++)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, (snd_pcm_format_t)f) == 0)
      printf(" %s", snd_pcm_format_name((snd_pcm_format_t)f));
  }
  printf("\n");

  unsigned int cmin = 0, cmax = 0;
  snd_pcm_hw_params_get_channels_min(hw, &cmin);
  snd_pcm_hw_params_get_channels_max(hw, &cmax);
  printf("channels: %u..%u\n", cmin, cmax);

  unsigned int rmin = 0, rmax = 0;
  int dir = 0;
  snd_pcm_hw_params_get_rate_min(hw, &rmin, &dir);
  snd_pcm_hw_params_get_rate_max(hw, &rmax, &dir);
  printf("rates: %u..%u Hz\n", rmin, rmax);

  snd_pcm_uframes_t pmin = 0, pmax = 0, bmin = 0, bmax = 0;
  snd_pcm_hw_params_get_period_size_min(hw, &pmin, &dir);
  snd_pcm_hw_params_get_period_size_max(hw, &pmax, &dir);
  snd_pcm_hw_params_get_buffer_size_min(hw, &bmin);
  snd_pcm_hw_params_get_buffer_size_max(hw, &bmax);
  printf("period: %lu..%lu frames\n",
         (unsigned long)pmin, (unsigned long)pmax);
  printf("buffer: %lu..%lu frames\n",
         (unsigned long)bmin, (unsigned long)bmax);

  snd_pcm_close(pcm);
  return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s [-D device] [-r rate] [-c channels] [-t seconds] out.wav\n"
          "       %s --probe [-D device]\n",
          argv0, argv0);
}

int main(int argc, char *argv[])
{
  const char *device = DEFAULT_DEVICE;
  const char *outpath = NULL;
  unsigned int rate = DEFAULT_RATE;
  unsigned int channels = DEFAULT_CHANNELS;
  double limit_seconds = 0.0; /* 0 = until Ctrl+C */
  int do_probe = 0;
  int opt;

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--probe") == 0)
    {
      do_probe = 1;
      argv[i] = (char *)"-P"; /* let getopt skip it harmlessly */
    }
  }

  while ((opt = getopt(argc, argv, "D:r:c:t:Ph")) != -1)
  {
    switch (opt)
    {
    case 'D':
      device = optarg;
      break;
    case 'r':
      rate = (unsigned)strtoul(optarg, NULL, 10);
      break;
    case 'c':
      channels = (unsigned)strtoul(optarg, NULL, 10);
      break;
    case 't':
      limit_seconds = strtod(optarg, NULL);
      break;
    case 'P':
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 2;
    }
  }

  if (do_probe)
    return probe_device(device);

  if (optind >= argc)
  {
    usage(argv[0]);
    return 2;
  }
  outpath = argv[optind];

  /* ---- open and configure the capture device ---- */

  snd_pcm_t *pcm = NULL;
  int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0)
  {
    fprintf(stderr, "cannot open %s: %s\n", device, snd_strerror(err));
    if (err == -EBUSY)
      fprintf(stderr, "device busy - PipeWire/PulseAudio may hold it. "
                      "Try -D plughw:CARD=Box,DEV=0\n");
    return 1;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);

  if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
  {
    fprintf(stderr, "hw_params_any: %s\n", snd_strerror(err));
    goto fail;
  }

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0)
  {
    fprintf(stderr, "set_access: %s\n", snd_strerror(err));
    goto fail;
  }

  /* pick the best format the hardware will actually accept */
  snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
  for (size_t i = 0; i < n_candidates; i++)
  {
    if (snd_pcm_hw_params_test_format(pcm, hw, candidate_formats[i]) == 0)
    {
      format = candidate_formats[i];
      break;
    }
  }
  if (format == SND_PCM_FORMAT_UNKNOWN)
  {
    fprintf(stderr, "no supported format found (run --probe)\n");
    goto fail;
  }
  if ((err = snd_pcm_hw_params_set_format(pcm, hw, format)) < 0)
  {
    fprintf(stderr, "set_format: %s\n", snd_strerror(err));
    goto fail;
  }

  if ((err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0)
  {
    fprintf(stderr, "set_channels(%u): %s\n", channels, snd_strerror(err));
    goto fail;
  }

  unsigned int actual_rate = rate;
  int dir = 0;
  err = snd_pcm_hw_params_set_rate_near(pcm, hw, &actual_rate, &dir);
  if (err < 0)
  {
    fprintf(stderr, "set_rate_near: %s\n", snd_strerror(err));
    goto fail;
  }
  if (actual_rate != rate)
    fprintf(stderr, "note: requested %u Hz, device gave %u Hz\n",
            rate, actual_rate);

  snd_pcm_uframes_t period = PERIOD_FRAMES;
  dir = 0;
  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir);

  snd_pcm_uframes_t buffer = period * N_PERIODS;
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

  if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
  {
    fprintf(stderr, "hw_params: %s\n", snd_strerror(err));
    goto fail;
  }

  snd_pcm_hw_params_get_period_size(hw, &period, &dir);
  snd_pcm_hw_params_get_buffer_size(hw, &buffer);

  unsigned hw_bytes = hw_bytes_for(format);
  unsigned wav_bits = wav_bits_for(format);
  unsigned wav_bytes = wav_bits / 8;
  int repack_s24 = (format == SND_PCM_FORMAT_S24_LE);

  fprintf(stderr,
          "recording: %s  %u Hz  %u ch  %s -> %u-bit WAV\n"
          "period %lu frames (%.1f ms), buffer %lu frames\n"
          "Ctrl+C to stop.\n",
          device, actual_rate, channels,
          snd_pcm_format_name(format), wav_bits,
          (unsigned long)period, 1000.0 * period / actual_rate,
          (unsigned long)buffer);

  /* ---- buffers and output file ---- */

  size_t hw_buf_bytes = (size_t)period * channels * hw_bytes;
  size_t out_buf_bytes = (size_t)period * channels * wav_bytes;

  unsigned char *hw_buf = malloc(hw_buf_bytes);
  unsigned char *out_buf = repack_s24 ? malloc(out_buf_bytes) : hw_buf;
  if (!hw_buf || !out_buf)
  {
    fprintf(stderr, "out of memory\n");
    goto fail;
  }

  FILE *f = fopen(outpath, "wb");
  if (!f)
  {
    perror(outpath);
    goto fail;
  }
  if (write_wav_header(f, 0, actual_rate,
                       (uint16_t)channels, (uint16_t)wav_bits) < 0)
  {
    perror("write header");
    fclose(f);
    goto fail;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  if ((err = snd_pcm_prepare(pcm)) < 0)
  {
    fprintf(stderr, "prepare: %s\n", snd_strerror(err));
    fclose(f);
    goto fail;
  }

  /* ---- capture loop ---- */

  uint64_t total_frames = 0;
  uint64_t total_bytes = 0;
  unsigned xruns = 0;
  unsigned meter_tick = 0;

  while (!g_stop)
  {
    snd_pcm_sframes_t got = snd_pcm_readi(pcm, hw_buf, period);

    if (got < 0)
    {
      if (got == -EPIPE)
        xruns++;
      got = snd_pcm_recover(pcm, (int)got, 1);
      if (got < 0)
      {
        fprintf(stderr, "\nread error: %s\n", snd_strerror((int)got));
        break;
      }
      continue;
    }
    if (got == 0)
      continue;

    size_t samples = (size_t)got * channels;

    if (repack_s24)
    {
      /* 4-byte container -> 3 packed little-endian bytes */
      for (size_t i = 0; i < samples; i++)
      {
        out_buf[i * 3 + 0] = hw_buf[i * 4 + 0];
        out_buf[i * 3 + 1] = hw_buf[i * 4 + 1];
        out_buf[i * 3 + 2] = hw_buf[i * 4 + 2];
      }
    }

    size_t nbytes = samples * wav_bytes;
    if (fwrite(out_buf, 1, nbytes, f) != nbytes)
    {
      perror("\nwrite");
      break;
    }

    total_frames += (uint64_t)got;
    total_bytes += nbytes;

    if (total_bytes >= MAX_DATA_BYTES)
    {
      fprintf(stderr, "\nreached WAV 4 GB limit, stopping\n");
      break;
    }

    double elapsed = (double)total_frames / actual_rate;
    if (limit_seconds > 0.0 && elapsed >= limit_seconds)
      break;

    if ((meter_tick++ % 4) == 0)
      draw_meter(peak_of(hw_buf, (snd_pcm_uframes_t)got,
                         channels, format),
                 elapsed, xruns);
  }

  /* ---- finalise ---- */

  snd_pcm_drain(pcm);

  if (write_wav_header(f, (uint32_t)total_bytes, actual_rate,
                       (uint16_t)channels, (uint16_t)wav_bits) < 0)
    perror("\npatch header");

  if (fflush(f) != 0 || fclose(f) != 0)
    perror("\nclose");

  fprintf(stderr, "\nwrote %s: %.2f s, %.1f MB, %u xruns\n",
          outpath, (double)total_frames / actual_rate,
          (double)total_bytes / (1024.0 * 1024.0), xruns);

  if (repack_s24)
    free(out_buf);
  free(hw_buf);
  snd_pcm_close(pcm);
  return 0;

fail:
  snd_pcm_close(pcm);
  return 1;
}
