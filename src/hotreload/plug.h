/* SPDX-License-Identifier: MIT */
/*
 * plug.h - the line the desktop app can be rebuilt across.
 *
 * The window is two programs. One is a shell that opens the window, runs the
 * capture thread and owns the run loop; the other is everything the app
 * actually does, and in a development build it is a shared library the shell
 * can throw away and load again while it is running. Pressing F5 rebuilds the
 * drawing, the timeline and the visualiser around a session that is still open
 * - the device stays open, the tracks stay where they are, and a take being
 * recorded keeps recording. It is the same idea, and most of the same shape, as
 * tsoding's musializer.
 *
 * These are the only calls that cross that line, listed once so the typedefs,
 * the pointers and the dlsym() names cannot drift apart: each of the three is
 * this list expanded through a different PLUG().
 *
 *   init         argv, the window, the device, and whatever was named on the
 *                command line. Returns 0 to run, or a process exit code; -1
 *                means it has already said its piece, as --help does.
 *   frame        one pass of the run loop: input, audio, and a drawn frame. It
 *                is told whether the window manager has asked to close, and
 *                answers whether to keep running - so an unsaved session can
 *                put a question up instead of going quietly.
 *   pre_reload   the library is about to go: let go of everything only this
 *                copy of the code knows the shape of, and hand back the state.
 *   post_reload  the new library takes that state as its own.
 *   shutdown     the window is closing.
 *
 * What may not change across a reload is the layout of anything the state
 * still holds when pre_reload returns - `app` itself above all. Editing app.h,
 * timeline.h or player.h means restarting; editing what the window draws, or
 * how it behaves, does not. See DESIGN.md.
 */
#ifndef AUDIAKI_PLUG_H
#define AUDIAKI_PLUG_H

#include <stdbool.h>

#define AUD_LIST_OF_PLUGS         \
  PLUG(init, int, int, char **)   \
  PLUG(frame, bool, bool)         \
  PLUG(pre_reload, void *, void)  \
  PLUG(post_reload, bool, void *) \
  PLUG(shutdown, void, void)

#define PLUG(name, ret, ...) typedef ret(aud_plug_##name##_t)(__VA_ARGS__);
AUD_LIST_OF_PLUGS
#undef PLUG

/* Defined once, in src/gui/plug.c, whichever way the app was built. */
#define PLUG(name, ret, ...) ret aud_plug_##name(__VA_ARGS__);
AUD_LIST_OF_PLUGS
#undef PLUG

#endif /* AUDIAKI_PLUG_H */
