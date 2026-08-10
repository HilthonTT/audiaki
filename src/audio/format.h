/* SPDX-License-Identifier: MIT */
/*
 * format.h - sample formats, peak detection and repacking.
 *
 * Deliberately free of any audio system dependency so the numeric code can be
 * unit tested on any host. Each backend owns the mapping to its own format
 * enum: device_alsa.c to snd_pcm_format_t, device_pipewire.c to spa_audio_format.
 */
#ifndef AUDIAKI_FORMAT_H
#define AUDIAKI_FORMAT_H

#include <stddef.h>

typedef enum
{
  AUD_FORMAT_UNKNOWN = 0,
  AUD_FORMAT_S16_LE,  /* 16 bit, 2 byte container      */
  AUD_FORMAT_S24_3LE, /* 24 bit, 3 byte container      */
  AUD_FORMAT_S24_LE,  /* 24 valid bits, 4 byte container */
  AUD_FORMAT_S32_LE,  /* 32 bit, 4 byte container      */
} aud_format;

/* Bytes per sample as delivered by the capture device. 0 if unknown. */
unsigned aud_format_hw_bytes(aud_format fmt);

/* Bits per sample once written to the WAV file. 0 if unknown. */
unsigned aud_format_wav_bits(aud_format fmt);

/* Bytes per sample once written to the WAV file. 0 if unknown. */
unsigned aud_format_wav_bytes(aud_format fmt);

/*
 * Non-zero when the captured layout differs from the WAV layout and the
 * frames must go through aud_format_repack() before being written.
 */
int aud_format_needs_repack(aud_format fmt);

/* Canonical lower-case name, e.g. "s24_3le". "unknown" if unrecognised. */
const char *aud_format_name(aud_format fmt);

/* Parse a canonical name (case insensitive). AUD_FORMAT_UNKNOWN on failure. */
aud_format aud_format_from_name(const char *name);

/*
 * Repack `samples` samples from the capture layout into the WAV layout.
 * Only meaningful when aud_format_needs_repack() is true; a no-op copy is
 * not performed for other formats, so callers must check first.
 *
 * dst must hold samples * aud_format_wav_bytes(fmt) bytes.
 * src must hold samples * aud_format_hw_bytes(fmt) bytes.
 * The buffers must not overlap.
 */
void aud_format_repack(void *dst, const void *src, size_t samples, aud_format fmt);

/*
 * Copy channel `channel` (0-based) out of an interleaved buffer, leaving the
 * samples in the capture layout.
 *
 * dst must hold frames * aud_format_hw_bytes(fmt) bytes. src must hold
 * frames * channels * that. The buffers must not overlap.
 *
 * Staying in the capture layout is what makes --channel cheap: the repack, the
 * peak, the spectrum and the WAV writer all then run unchanged with a channel
 * count of one, rather than each growing a second path that knows about picked
 * channels.
 *
 * Writes nothing when `channel` is out of range, so a caller that has not
 * checked it against the device produces silence rather than reading past the
 * end of a frame.
 */
void aud_format_pick_channel(void *dst, const void *src, size_t frames, unsigned channels,
                             unsigned channel, aud_format fmt);

/*
 * Average every channel of an interleaved buffer down to one, leaving the
 * samples in the capture layout.
 *
 * The counterpart to aud_format_pick_channel() and interchangeable with it:
 * both turn an interleaved period into one channel's worth in the same layout,
 * so everything downstream - the repack, the peak, the spectrum, the WAV
 * writer - runs unchanged with a channel count of one either way.
 *
 * Averaged rather than summed, so a mixdown cannot clip: the mean of a set of
 * samples is never further from silence than the furthest of them. That costs
 * up to 3 dB against a summed mix on material that is the same in every
 * channel, which is the right trade for a capture path where a clipped take
 * cannot be undone.
 *
 * dst must hold frames * aud_format_hw_bytes(fmt) bytes. src must hold
 * frames * channels * that. The buffers must not overlap.
 */
void aud_format_mix_channels(void *dst, const void *src, size_t frames, unsigned channels,
                             aud_format fmt);

/*
 * Absolute peak of an interleaved buffer, normalised to [0.0, 1.0].
 * Returns 0.0 for unknown formats or empty buffers.
 */
double aud_format_peak(const void *buf, size_t frames, unsigned channels, aud_format fmt);

/*
 * Decode `frames` interleaved frames into mono floats in [-1.0, 1.0),
 * averaging the channels. dst must hold `frames` floats.
 *
 * Written for the spectrum analyser, which does not care about channel
 * separation. Unknown formats fill dst with zeros.
 */
void aud_format_to_mono(float *dst, const void *src, size_t frames, unsigned channels,
                        aud_format fmt);

/*
 * Decode `frames` interleaved frames into interleaved floats in [-1.0, 1.0),
 * keeping the channels apart. dst must hold frames * channels floats.
 *
 * Written for playback monitoring, which has to put the left channel back in
 * the left ear. Unknown formats fill dst with zeros.
 */
void aud_format_to_float(float *dst, const void *src, size_t frames, unsigned channels,
                         aud_format fmt);

/*
 * The peak at which a buffer counts as clipped.
 *
 * Not 1.0. Signed PCM is asymmetric: the most negative sample normalises to
 * exactly -1.0, but the most positive is one step short - 32767/32768 for
 * 16 bit, and closer still at 32 - so a test against 1.0 catches a take that
 * clipped downwards and silently misses one that clipped upwards.
 */
#define AUD_CLIP_THRESHOLD 0.999

/* Convert a normalised peak to dBFS, clamped at AUD_DBFS_FLOOR. */
#define AUD_DBFS_FLOOR (-99.0)
double aud_format_dbfs(double peak);

#endif /* AUDIAKI_FORMAT_H */
