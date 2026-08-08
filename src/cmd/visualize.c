/* SPDX-License-Identifier: MIT */
/*
 * --visualize: turn a take into a video.
 *
 * The renderer is media/visualize.c, which opens no audio device - it reads a
 * WAV and pipes frames to ffmpeg. This is the part that works out what to call
 * the output and refuses to write over the wrong thing.
 */
#include "cmd/cmd.h"

#include "media/visualize.h"
#include "take/take.h"
#include "util/log.h"
#include "util/signals.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int aud_cmd_visualize(const aud_options *opts)
{
  aud_visualize_options viz;
  char derived[4096];

  aud_visualize_defaults(&viz);
  viz.input_path = opts->input_path;
  viz.output_path = opts->output_path;
  viz.width = opts->viz_width;
  viz.height = opts->viz_height;
  viz.fps = opts->viz_fps;
  viz.bars = opts->viz_bars;
  viz.style = opts->viz_style;

  /* default output: the input with its extension replaced by .mp4 */
  if (viz.output_path == NULL)
  {
    if (aud_take_with_extension(derived, sizeof(derived), opts->input_path, ".mp4") != 0)
    {
      aud_error("input path is too long to derive an output name from");
      aud_info("pass the video name with -o");
      return EXIT_FAILURE;
    }
    viz.output_path = derived;
  }

  if (strcmp(viz.input_path, viz.output_path) == 0)
  {
    aud_error("%s is both the input and the output", viz.input_path);
    return EXIT_FAILURE;
  }

  /* the renderer overwrites, so this check is the one that enforces --force */
  if (!opts->overwrite && access(viz.output_path, F_OK) == 0)
  {
    aud_error("%s already exists (pass --force to overwrite)", viz.output_path);
    return EXIT_FAILURE;
  }

  if (aud_signals_install_stop() != 0)
  {
    aud_perror("cannot install signal handlers");
    return EXIT_FAILURE;
  }

  return aud_visualize_render(&viz) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
