/* SPDX-License-Identifier: MIT */
/*
 * chooser.h - the desktop's own file dialog, when there is one.
 *
 * audiaki's built-in browser knows how to walk folders and nothing else: no
 * bookmarks, no recent places, no search, and no idea what the desktop's own
 * chooser has been configured to do. For picking where a take goes that is
 * usually enough, and it is always there - but it is not what anybody's other
 * applications look like.
 *
 * So when the desktop can offer a real one, this hands off to it. `zenity` and
 * `kdialog` are the two front ends worth having: both are the desktop's actual
 * chooser rather than an imitation, and under a portal - Flatpak, or a sandbox
 * - both go through xdg-desktop-portal, which is the one that gets it right.
 * When neither is installed the built-in browser is still there, which is why
 * this is allowed to be absent rather than being a dependency.
 *
 * -- why it does not block --
 *
 * The window is a render loop. Running a modal chooser to completion inside it
 * would stop the loop for as long as somebody is browsing, and a window that
 * has stopped drawing is one the compositor greys out and the desktop offers
 * to kill. So the child is started, and polled once a frame: the loop keeps
 * running, the take keeps recording, and the answer arrives when it arrives.
 */
#ifndef AUDIAKI_GUI_CHOOSER_H
#define AUDIAKI_GUI_CHOOSER_H

#include <stddef.h>

typedef struct aud_chooser aud_chooser;

typedef enum
{
  AUD_CHOOSER_SAVE = 0, /* pick a name to write, warning about overwrites */
  AUD_CHOOSER_OPEN,     /* pick a file that is already there */
  AUD_CHOOSER_FOLDER,   /* pick a directory */
} aud_chooser_mode;

/*
 * Non-zero when a system chooser can be run at all. Checked before offering
 * the button, so a machine without one never shows something that would do
 * nothing.
 *
 * $AUDIAKI_FILE_CHOOSER names one explicitly - "zenity", "kdialog", or "none"
 * to keep the built-in browser whatever else is installed.
 */
int aud_chooser_available(void);

/*
 * Start one. `dir` is the folder to open in and `name` the filename to suggest
 * (ignored except when saving); either may be NULL. `filter` is a glob such as
 * "*.wav", or NULL for everything.
 *
 * Returns NULL if no chooser could be started, at which point the caller
 * should carry on with the built-in browser.
 */
aud_chooser *aud_chooser_start(aud_chooser_mode mode, const char *title, const char *dir,
                               const char *name, const char *filter);

/*
 * How a chooser is getting on. Returns 0 while it is still up, 1 once a path
 * has been chosen - written into `out` - and -1 when it was cancelled or
 * failed. The handle stays valid either way; the caller frees it with
 * aud_chooser_close().
 */
int aud_chooser_poll(aud_chooser *c, char *out, size_t size);

/* Close the handle, killing the chooser if it is still up. */
void aud_chooser_close(aud_chooser *c);

#endif /* AUDIAKI_GUI_CHOOSER_H */
