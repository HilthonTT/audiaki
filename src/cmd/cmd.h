/* SPDX-License-Identifier: MIT */
/*
 * cmd.h - the commands an invocation can name.
 *
 * One translation unit each, all the same shape: take the parsed options, do
 * the work, hand back a process exit code. main.c does nothing but pick one.
 *
 * This is the layer that drives src/backend, and the only one that does. That
 * is why everything below it builds and tests on a machine with no sound
 * server at all - see PORTABLE_SRCS in the Makefile, which says exactly that
 * as a directory rule.
 */
#ifndef AUDIAKI_CMD_H
#define AUDIAKI_CMD_H

#include "backend/device.h"
#include "options.h"

int aud_cmd_record(const aud_options *opts);
int aud_cmd_play(const aud_options *opts);
int aud_cmd_tune(const aud_options *opts);
int aud_cmd_info(const aud_options *opts);
int aud_cmd_visualize(const aud_options *opts);
int aud_cmd_render(const aud_options *opts);
int aud_cmd_list(const aud_options *opts);
int aud_cmd_probe(const aud_options *opts);

/*
 * The capture geometry -D, -r, -c, -f, -p and -n describe. Shared because
 * --record and --tune open the same kind of device from the same options, and
 * two copies of the mapping is two places for one of them to fall behind.
 */
void aud_cmd_capture_config(aud_device_config *cfg, const aud_options *opts);

#endif /* AUDIAKI_CMD_H */
