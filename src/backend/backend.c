/* SPDX-License-Identifier: MIT */
/*
 * backend.c - which audio system this process talks to.
 *
 * One choice, made once, before anything is opened. Everything after it goes
 * through the tables this file hands out.
 */
#include "backend/backend.h"

#include "util/log.h"

#include <string.h>

static const aud_capture_ops *capture_ops = &aud_capture_ops_alsa;
static const aud_monitor_ops *monitor_ops = &aud_monitor_ops_alsa;

int aud_backend_parse(const char *name, aud_backend_kind *out)
{
  if (name == NULL || out == NULL)
  {
    return -1;
  }

  if (strcmp(name, "auto") == 0)
  {
    *out = AUD_BACKEND_AUTO;
    return 0;
  }
  if (strcmp(name, "alsa") == 0)
  {
    *out = AUD_BACKEND_ALSA;
    return 0;
  }
  if (strcmp(name, "pipewire") == 0 || strcmp(name, "pw") == 0)
  {
    *out = AUD_BACKEND_PIPEWIRE;
    return 0;
  }
  return -1;
}

const char *aud_backend_name(aud_backend_kind kind)
{
  switch (kind)
  {
  case AUD_BACKEND_ALSA:
    return "alsa";
  case AUD_BACKEND_PIPEWIRE:
    return "pipewire";
  case AUD_BACKEND_AUTO:
  default:
    return "auto";
  }
}

int aud_backend_available(aud_backend_kind kind)
{
  switch (kind)
  {
  case AUD_BACKEND_ALSA:
    return 1;
  case AUD_BACKEND_PIPEWIRE:
#ifdef AUDIAKI_HAVE_PIPEWIRE
    /*
     * Compiled in is not the same as running. The library is present on plenty
     * of machines where the daemon is not, and "available" has to mean the
     * thing will actually open or auto-selection would strand them.
     */
    return aud_pipewire_daemon_responds();
#else
    return 0;
#endif
  case AUD_BACKEND_AUTO:
  default:
    return 1;
  }
}

int aud_backend_select(aud_backend_kind kind)
{
  if (kind == AUD_BACKEND_AUTO)
  {
    /*
     * PipeWire first when it answers. On a desktop it is what owns the card,
     * and going through it is how audiaki coexists with everything else that
     * wants the same interface. ALSA is the answer everywhere else, and stays
     * the answer for anyone who asks for it by name.
     */
    kind = aud_backend_available(AUD_BACKEND_PIPEWIRE) ? AUD_BACKEND_PIPEWIRE
                                                       : AUD_BACKEND_ALSA;
    aud_debug("backend: auto-selected %s", aud_backend_name(kind));
  }
  else if (!aud_backend_available(kind))
  {
    /*
     * Not a silent downgrade: someone who typed --backend pipewire wants to
     * hear that it was not there, not to discover later that their device
     * names came from somewhere else.
     */
#ifdef AUDIAKI_HAVE_PIPEWIRE
    aud_error("the %s backend is not answering; is the daemon running?",
              aud_backend_name(kind));
#else
    aud_error("this build has no %s backend (it was compiled without the "
              "PipeWire development headers)",
              aud_backend_name(kind));
#endif
    return -1;
  }

  switch (kind)
  {
#ifdef AUDIAKI_HAVE_PIPEWIRE
  case AUD_BACKEND_PIPEWIRE:
    capture_ops = &aud_capture_ops_pipewire;
    monitor_ops = &aud_monitor_ops_pipewire;
    break;
#endif
  case AUD_BACKEND_ALSA:
  default:
    capture_ops = &aud_capture_ops_alsa;
    monitor_ops = &aud_monitor_ops_alsa;
    break;
  }

  return 0;
}

const aud_capture_ops *aud_backend_capture(void)
{
  return capture_ops;
}

const aud_monitor_ops *aud_backend_monitor(void)
{
  return monitor_ops;
}
