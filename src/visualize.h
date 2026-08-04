/* SPDX-License-Identifier: MIT */
/*
 * visualize.h - render a WAV file into a spectrum visualiser video.
 *
 * Reads the take, analyses a window of it per video frame, rasterises bars into
 * an RGBA canvas and pipes the frames to ffmpeg, which muxes them with the
 * original audio. Nothing here touches ALSA; a render does not need a sound
 * card and can run on a machine that has never seen the interface.
 */
#ifndef AUDIAKI_VISUALIZE_H
#define AUDIAKI_VISUALIZE_H

#include <stddef.h>

#define AUD_VIZ_DEFAULT_WIDTH 1280u
#define AUD_VIZ_DEFAULT_HEIGHT 720u
#define AUD_VIZ_DEFAULT_FPS 60u
#define AUD_VIZ_DEFAULT_BARS 64u

/*
 * Note there is no overwrite flag: ffmpeg is always told to replace the output.
 * Enforcing "refuse unless --force" is the caller's job, so that the message
 * matches the one recording gives.
 */
typedef struct
{
  const char *input_path;  /* WAV to read */
  const char *output_path; /* video to write; the container follows the suffix */
  unsigned width;
  unsigned height;
  unsigned fps;
  unsigned bars;
} aud_visualize_options;

void aud_visualize_defaults(aud_visualize_options *opts);

/*
 * Render the video. Returns 0 on success, -1 after reporting the reason
 * through log.h. Ctrl+C cancels: ffmpeg is killed and the partial output is
 * removed, and that counts as success.
 */
int aud_visualize_render(const aud_visualize_options *opts);

#endif /* AUDIAKI_VISUALIZE_H */
