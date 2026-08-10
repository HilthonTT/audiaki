/* SPDX-License-Identifier: MIT */
/*
 * audiaki-gui - the shell the window runs inside.
 *
 * All this does is hold the things that must not be thrown away: the window
 * itself, the capture thread, and the run loop. Everything the app is - the
 * drawing, the timeline, the transport, the visualiser - is behind the five
 * calls in hotreload/plug.h, and in a development build it is a library this
 * process can drop and load again between two frames.
 *
 * That is what F5 does. The session stays open across it because the state is
 * on the heap and is handed from the old library to the new one, and the audio
 * never stops because the device and the thread reading it are here, in the
 * part that is never unloaded. A take that is recording keeps recording while
 * the code drawing it is replaced underneath.
 *
 * In a release build there is no library and no dlopen: AUD_PLUG() resolves
 * straight to the functions, and this is just a main() with a loop in it.
 */
#include "hotreload/hotreload.h"

#include "util/log.h"

#include "raylib.h"

#include <stdlib.h>

int main(int argc, char *argv[])
{
  int rc;

  if (!aud_hotreload_load())
  {
    return EXIT_FAILURE;
  }

  rc = AUD_PLUG(init)(argc, argv);
  if (rc != 0)
  {
    /* below zero is --help and its like: answered, and nothing went wrong */
    return rc < 0 ? EXIT_SUCCESS : rc;
  }

  /*
   * The close request is handed to the app rather than acted on here, because
   * whether it is safe to go is a question about the session and the session is
   * the app's. raylib clears the flag every frame, so a close that is declined
   * simply does not happen and the next one asks again.
   */
  for (;;)
  {
#ifdef AUDIAKI_HOTRELOAD
    if (IsKeyPressed(KEY_F5))
    {
      void *state = AUD_PLUG(pre_reload)();

      /*
       * A load that fails leaves the entry points pointing where they already
       * pointed, so post_reload puts the library that is still running back
       * together with its own state and the window carries on. Which is the
       * whole reason the reload is attempted before anything is closed: a
       * build that does not compile should cost a line on the terminal, not
       * the session.
       */
      (void)aud_hotreload_load();

      if (!AUD_PLUG(post_reload)(state))
      {
        aud_error("the reloaded window would not take the session; stopping");
        return EXIT_FAILURE;
      }
    }
#endif

    if (!AUD_PLUG(frame)(WindowShouldClose()))
    {
      break;
    }
  }

  AUD_PLUG(shutdown)();
  return EXIT_SUCCESS;
}
