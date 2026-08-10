/* SPDX-License-Identifier: MIT */
/*
 * backend.h - the audio system audiaki talks to.
 *
 * There are four. ALSA opens the card directly. PipeWire asks the sound server
 * that owns the card on most current desktops. JACK joins a graph somebody else
 * set up, where the capture device is another program's port as often as it is
 * an interface. CoreAudio is the same job again on macOS, where it is the only
 * audio system there is. They answer the same questions - what capture devices
 * exist, open one, hand me frames - so everything above this header works in
 * terms of aud_device and aud_monitor and never learns which one it got.
 *
 * The tables are filled in by device_alsa.c / monitor_alsa.c and the three sets
 * of counterparts. A backend that was not compiled in is simply absent here,
 * which is how a machine without the PipeWire headers still builds, and how the
 * same source builds on macOS with no ALSA on it at all: the selection in
 * backend.c reports it as unavailable rather than failing to link.
 */
#ifndef AUDIAKI_BACKEND_H
#define AUDIAKI_BACKEND_H

#include "audio/format.h"

#include <stddef.h>

typedef enum
{
  AUD_BACKEND_AUTO = 0, /* the first of the others that answers; see backend.c */
  AUD_BACKEND_ALSA,
  AUD_BACKEND_PIPEWIRE,
  AUD_BACKEND_JACK,
  AUD_BACKEND_COREAUDIO
} aud_backend_kind;

/* Forward declarations; the full types live in device.h and monitor.h. */
typedef struct aud_device aud_device;
typedef struct aud_device_config aud_device_config;
typedef struct aud_device_entry aud_device_entry;
typedef struct aud_device_watch aud_device_watch;
typedef struct aud_monitor_config aud_monitor_config;

/*
 * Capture. Each entry matches the aud_device_* function that dispatches to it,
 * so device.c reads as a table of one-line forwards and the contract for an
 * implementation is the documentation already written in device.h.
 */
typedef struct
{
  const char
      *name; /* "alsa", "pipewire", "jack" or "coreaudio", as --backend spells it */

  int (*open_capture)(aud_device *dev, const aud_device_config *cfg);
  void (*close)(aud_device *dev);
  long (*read)(aud_device *dev, void *buf, unsigned long frames, unsigned *xruns);
  void (*drop)(aud_device *dev); /* discard what is queued, at the end of a take */

  int (*probe)(const char *name, int json);
  int (*enumerate)(aud_device_entry **out);

  /* The watch handle is the implementation's own; device.c wraps it. */
  void *(*watch_create)(void);
  void (*watch_destroy)(void *impl);
  int (*watch_changed)(void *impl);
} aud_capture_ops;

/* Playback. `open` returns the implementation's own handle. */
typedef struct
{
  const char *name;

  void *(*open)(const aud_monitor_config *cfg, unsigned *rate_out,
                unsigned *channels_out);
  void (*close)(void *impl);
  int (*write)(void *impl, const float *interleaved, size_t frames, float gain);
  unsigned long (*dropped)(const void *impl);

  /* What a caller feeding a file rather than a live capture needs; see monitor.h */
  long (*space)(void *impl);
  void (*drain)(void *impl);
  void (*flush)(void *impl);
} aud_monitor_ops;

/*
 * Resolve a backend name to a kind. Accepts "auto", "alsa", "pipewire" (or
 * "pw"), "jack" and "coreaudio" (or "ca"), whether or not this build has them.
 * Returns 0 on success and -1 if the name is not one of those.
 *
 * A name this build was compiled without still parses, so --backend jack on a
 * build without the headers fails at selection with a message that says so,
 * rather than as an unknown option.
 */
int aud_backend_parse(const char *name, aud_backend_kind *out);

/* The canonical name, for help text and diagnostics. */
const char *aud_backend_name(aud_backend_kind kind);

/*
 * The backends this build has, as "auto, pipewire or alsa" - ready to drop into
 * help text and into the message for a --backend nobody here recognises.
 *
 * Only the ones compiled in, because the alternative is a --help that offers
 * jack on a build that cannot do it. Which four names are valid does not vary,
 * but which of them will work does, and the list is there to answer the second
 * question.
 */
const char *aud_backend_list(void);

/*
 * Choose the backend for this process. Call once, before opening anything;
 * calling it again after a stream is open does not move that stream.
 *
 * AUD_BACKEND_AUTO takes the first of PipeWire, CoreAudio, ALSA and JACK that
 * this build has and that answers. Asking for a backend that is not available
 * is an error rather than a silent downgrade: someone who passed
 * --backend pipewire wants to know it was not there.
 *
 * Returns 0 on success and -1 after reporting the reason through log.h.
 */
int aud_backend_select(aud_backend_kind kind);

/* Whether `kind` could be selected: compiled in, and answering if it is a server. */
int aud_backend_available(aud_backend_kind kind);

/* Whether this build has `kind` at all, without asking a daemon anything. */
int aud_backend_compiled_in(aud_backend_kind kind);

/*
 * The tables in use. Never NULL once aud_backend_select() has returned 0; both
 * start at the platform's native backend before it is called, so a caller that
 * never selects gets exactly the behaviour audiaki had before backends existed.
 */
const aud_capture_ops *aud_backend_capture(void);
const aud_monitor_ops *aud_backend_monitor(void);

/* The implementations. Each pair exists only where its backend was compiled in. */
#ifdef AUDIAKI_HAVE_ALSA
extern const aud_capture_ops aud_capture_ops_alsa;
extern const aud_monitor_ops aud_monitor_ops_alsa;
#endif

#ifdef AUDIAKI_HAVE_PIPEWIRE
extern const aud_capture_ops aud_capture_ops_pipewire;
extern const aud_monitor_ops aud_monitor_ops_pipewire;

/*
 * Whether a PipeWire daemon is there to talk to. Connects, waits briefly for
 * the server to say hello, and disconnects; the answer is cached, because
 * auto-selection and --list would otherwise pay for it twice.
 */
int aud_pipewire_daemon_responds(void);
#endif

#ifdef AUDIAKI_HAVE_JACK
extern const aud_capture_ops aud_capture_ops_jack;
extern const aud_monitor_ops aud_monitor_ops_jack;

/*
 * Whether a JACK server is there to join. Opens a client with the "do not start
 * a server" option and closes it again; cached, for the same reason the
 * PipeWire probe is.
 *
 * The option matters more here than it looks: without it libjack will spawn a
 * daemon of its own, and a --list that silently starts a sound server and takes
 * the card off whatever had it is not a listing.
 */
int aud_jack_server_responds(void);
#endif

#ifdef AUDIAKI_HAVE_COREAUDIO
extern const aud_capture_ops aud_capture_ops_coreaudio;
extern const aud_monitor_ops aud_monitor_ops_coreaudio;
#endif

#endif /* AUDIAKI_BACKEND_H */
