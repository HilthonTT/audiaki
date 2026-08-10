/* SPDX-License-Identifier: MIT */
/*
 * backend.c - which audio system this process talks to.
 *
 * One choice, made once, before anything is opened. Everything after it goes
 * through the tables this file hands out.
 *
 * Four backends and two platforms, so the knowledge is in one table rather than
 * spread over a switch per question. A row whose ops are NULL is a backend this
 * build does not have, and that is the only difference between "PipeWire is not
 * installed here" and "ALSA does not exist on macOS": nothing below the table
 * needs to tell those apart.
 */
#include "backend/backend.h"

#include "util/log.h"

#include <stdio.h>
#include <string.h>

typedef struct
{
  aud_backend_kind kind;
  const char *name;
  const char *alias; /* a second spelling --backend accepts, or NULL */
  const aud_capture_ops *capture;
  const aud_monitor_ops *monitor;
  /*
   * Whether the thing is there to talk to right now. NULL means "it is there
   * if it compiled", which is the answer for a backend that opens the hardware
   * itself rather than asking a daemon.
   */
  int (*responds)(void);
} backend_row;

/*
 * Auto-selection order, which is the order of this table.
 *
 * PipeWire first where it answers: on a current desktop it owns the card, and
 * going through it is how audiaki coexists with everything else that wants the
 * same interface. CoreAudio next, which on macOS is not a preference but the
 * only answer. Then ALSA, the answer on a Linux machine with no sound server.
 *
 * JACK last, and in practice only when it is the sole backend compiled in.
 * That is deliberate: a JACK graph is something somebody wired up on purpose,
 * where the right capture source is a decision rather than a default, and on a
 * desktop running pipewire-jack a JACK server "answering" says nothing about
 * what the user wanted. Asking for it by name is the way in.
 */
static const backend_row backends[] = {
#ifdef AUDIAKI_HAVE_PIPEWIRE
    {AUD_BACKEND_PIPEWIRE, "pipewire", "pw", &aud_capture_ops_pipewire,
     &aud_monitor_ops_pipewire, aud_pipewire_daemon_responds},
#else
    {AUD_BACKEND_PIPEWIRE, "pipewire", "pw", NULL, NULL, NULL},
#endif
#ifdef AUDIAKI_HAVE_COREAUDIO
    {AUD_BACKEND_COREAUDIO, "coreaudio", "ca", &aud_capture_ops_coreaudio,
     &aud_monitor_ops_coreaudio, NULL},
#else
    {AUD_BACKEND_COREAUDIO, "coreaudio", "ca", NULL, NULL, NULL},
#endif
#ifdef AUDIAKI_HAVE_ALSA
    {AUD_BACKEND_ALSA, "alsa", NULL, &aud_capture_ops_alsa, &aud_monitor_ops_alsa, NULL},
#else
    {AUD_BACKEND_ALSA, "alsa", NULL, NULL, NULL, NULL},
#endif
#ifdef AUDIAKI_HAVE_JACK
    {AUD_BACKEND_JACK, "jack", NULL, &aud_capture_ops_jack, &aud_monitor_ops_jack,
     aud_jack_server_responds},
#else
    {AUD_BACKEND_JACK, "jack", NULL, NULL, NULL, NULL},
#endif
};

#define BACKEND_COUNT (sizeof(backends) / sizeof(backends[0]))

#if !defined(AUDIAKI_HAVE_ALSA) && !defined(AUDIAKI_HAVE_PIPEWIRE) && \
    !defined(AUDIAKI_HAVE_JACK) && !defined(AUDIAKI_HAVE_COREAUDIO)
#error "no audio backend was compiled in; see the Makefile and scripts/install-deps.sh"
#endif

/*
 * What a caller that never selects gets: the platform's native backend, which
 * is what audiaki used before any of this was a choice. The order matches the
 * table's, minus the daemon probes - this is a file-scope initialiser, so it
 * cannot ask anything.
 */
#if defined(AUDIAKI_HAVE_ALSA)
static const aud_capture_ops *capture_ops = &aud_capture_ops_alsa;
static const aud_monitor_ops *monitor_ops = &aud_monitor_ops_alsa;
#elif defined(AUDIAKI_HAVE_COREAUDIO)
static const aud_capture_ops *capture_ops = &aud_capture_ops_coreaudio;
static const aud_monitor_ops *monitor_ops = &aud_monitor_ops_coreaudio;
#elif defined(AUDIAKI_HAVE_PIPEWIRE)
static const aud_capture_ops *capture_ops = &aud_capture_ops_pipewire;
static const aud_monitor_ops *monitor_ops = &aud_monitor_ops_pipewire;
#else
static const aud_capture_ops *capture_ops = &aud_capture_ops_jack;
static const aud_monitor_ops *monitor_ops = &aud_monitor_ops_jack;
#endif

static const backend_row *row_for(aud_backend_kind kind)
{
  for (size_t i = 0; i < BACKEND_COUNT; i++)
  {
    if (backends[i].kind == kind)
    {
      return &backends[i];
    }
  }
  return NULL;
}

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

  /*
   * Every name this file knows, not only the ones this build has. A build
   * without the JACK headers should fail --backend jack with "this build has
   * no jack backend" rather than "invalid value", which reads as a typo.
   */
  for (size_t i = 0; i < BACKEND_COUNT; i++)
  {
    if (strcmp(name, backends[i].name) == 0 ||
        (backends[i].alias != NULL && strcmp(name, backends[i].alias) == 0))
    {
      *out = backends[i].kind;
      return 0;
    }
  }
  return -1;
}

const char *aud_backend_name(aud_backend_kind kind)
{
  const backend_row *row = row_for(kind);

  return row != NULL ? row->name : "auto";
}

const char *aud_backend_list(void)
{
  /*
   * Built once into a buffer big enough for every name plus its separator, and
   * the sizes are the compiler's arithmetic rather than a number to keep in
   * step: adding a fifth backend to the table above extends this by itself.
   */
  static char list[16 + BACKEND_COUNT * 32];

  if (list[0] == '\0')
  {
    size_t used = 0;
    int count = 0;

    /* how many there are decides "a or b" against "a, b or c" */
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
      count += backends[i].capture != NULL;
    }

    used += (size_t)snprintf(list, sizeof(list), "auto");

    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
      if (backends[i].capture == NULL)
      {
        continue;
      }

      used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s",
                               count-- > 1 ? ", " : " or ", backends[i].name);
    }
  }

  return list;
}

int aud_backend_compiled_in(aud_backend_kind kind)
{
  const backend_row *row;

  if (kind == AUD_BACKEND_AUTO)
  {
    return 1;
  }

  row = row_for(kind);
  return row != NULL && row->capture != NULL;
}

int aud_backend_available(aud_backend_kind kind)
{
  const backend_row *row;

  if (kind == AUD_BACKEND_AUTO)
  {
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
      if (aud_backend_available(backends[i].kind))
      {
        return 1;
      }
    }
    return 0;
  }

  row = row_for(kind);
  if (row == NULL || row->capture == NULL)
  {
    return 0;
  }

  /*
   * Compiled in is not the same as running. A sound server's library is present
   * on plenty of machines where its daemon is not, and "available" has to mean
   * the thing will actually open or auto-selection would strand them.
   */
  return row->responds == NULL || row->responds();
}

int aud_backend_select(aud_backend_kind kind)
{
  const backend_row *row;

  if (kind == AUD_BACKEND_AUTO)
  {
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
      if (aud_backend_available(backends[i].kind))
      {
        kind = backends[i].kind;
        break;
      }
    }

    if (kind == AUD_BACKEND_AUTO)
    {
      aud_error("no audio backend is available on this machine");
      return -1;
    }
    aud_debug("backend: auto-selected %s", aud_backend_name(kind));
  }
  else if (!aud_backend_available(kind))
  {
    /*
     * Not a silent downgrade: someone who typed --backend pipewire wants to
     * hear that it was not there, not to discover later that their device
     * names came from somewhere else.
     */
    if (!aud_backend_compiled_in(kind))
    {
      /*
       * Without saying why. There are two reasons - the development headers
       * were not installed, or the backend does not exist on this platform at
       * all - and guessing at which produces "compiled without the alsa headers"
       * on a Mac, where no such headers were ever available to install.
       */
      aud_error("this build has no %s backend", aud_backend_name(kind));
      aud_info("it has: %s", aud_backend_list());
    }
    else
    {
      aud_error("the %s backend is not answering; is the daemon running?",
                aud_backend_name(kind));
    }
    return -1;
  }

  row = row_for(kind);
  if (row == NULL || row->capture == NULL)
  {
    return -1;
  }

  capture_ops = row->capture;
  monitor_ops = row->monitor;
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
