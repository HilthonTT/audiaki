/* SPDX-License-Identifier: MIT */
/*
 * signals.h - one Ctrl+C flag, shared by everything that runs a long loop.
 *
 * Both the capture loop and the video renderer need SIGINT to stop them
 * cleanly rather than kill them mid-file. This lives on its own so the
 * renderer does not have to include recorder.h, and with it all of ALSA.
 */
#ifndef AUDIAKI_SIGNALS_H
#define AUDIAKI_SIGNALS_H

/*
 * Install handlers for SIGINT and SIGTERM that set the stop flag.
 * Returns 0 on success, -1 with errno set.
 *
 * Deliberately without SA_RESTART: a blocking read must return EINTR so the
 * loop notices the request instead of waiting for another period of audio.
 */
int aud_signals_install_stop(void);

/* Non-zero once SIGINT or SIGTERM has arrived. */
int aud_signals_stop_requested(void);

#endif /* AUDIAKI_SIGNALS_H */
