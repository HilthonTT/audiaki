/* SPDX-License-Identifier: MIT */
/*
 * hotreload.h - how the shell reaches the app.
 *
 * Only src/gui/main.c includes this. With AUDIAKI_HOTRELOAD the entry points
 * are pointers into a library that is loaded now and may be a different one in
 * a moment; without it they are ordinary functions in the same binary, and
 * every call site reads the same either way:
 *
 *   AUD_PLUG(frame)();
 *
 * The release build is the one with no dlopen in it at all. Hot reloading is a
 * development convenience, not something a user should be paying a level of
 * indirection - or a second file next to the binary - for.
 */
#ifndef AUDIAKI_HOTRELOAD_H
#define AUDIAKI_HOTRELOAD_H

#include "hotreload/plug.h"

#include <stdbool.h>

#ifdef AUDIAKI_HOTRELOAD

#define PLUG(name, ...) extern aud_plug_##name##_t *aud_hot_##name;
AUD_LIST_OF_PLUGS
#undef PLUG

#define AUD_PLUG(name) (*aud_hot_##name)

/*
 * Load the app's library and point the entry points at it. The first call
 * brings it up; every call after that is a reload.
 *
 * A reload that fails - which nearly always means the build it was reaching
 * for is broken - says why and returns false with the library that is already
 * running left exactly as it was, so a typo costs a message rather than the
 * session. Only the first call is fatal to the caller, there being nothing to
 * fall back to.
 */
bool aud_hotreload_load(void);

#else

#define AUD_PLUG(name) aud_plug_##name

#define aud_hotreload_load() true

#endif /* AUDIAKI_HOTRELOAD */

#endif /* AUDIAKI_HOTRELOAD_H */
