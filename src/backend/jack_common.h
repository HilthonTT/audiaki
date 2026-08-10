/* SPDX-License-Identifier: MIT */
/*
 * jack_common.h - what the two JACK backends both need.
 *
 * Internal to src/backend, and to the JACK half of it: opening a client on the
 * graph without starting a server, and taking a port name apart. device_jack.c
 * and monitor_jack.c each do their own job with these; nothing outside those
 * two files includes this.
 *
 * Inline in a header rather than a third translation unit because that is all
 * this is - five short functions with no state between them, where a .c file
 * would mean a build rule and an object for no gain.
 */
#ifndef AUDIAKI_JACK_COMMON_H
#define AUDIAKI_JACK_COMMON_H

#include "util/log.h"
#include "version.h"

#include <jack/jack.h>

#include <pthread.h>
#include <string.h>

/* What audiaki's clients are called on the graph, before their purpose is added. */
#define JACK_CLIENT_PREFIX AUDIAKI_NAME

/*
 * libjack prints to stderr of its own accord, and cheerfully - "cannot connect
 * to server socket" is a sentence audiaki wants to say itself, once, in its own
 * words. Routing both channels to the debug log keeps them available under -v
 * without them landing on top of a --list.
 */
static inline void aud_jack_quiet_error(const char *message)
{
  aud_debug("jack: %s", message != NULL ? message : "");
}

static inline void aud_jack_quiet_info(const char *message)
{
  aud_debug("jack: %s", message != NULL ? message : "");
}

static inline void aud_jack_quiet_call(void)
{
  jack_set_error_function(aud_jack_quiet_error);
  jack_set_info_function(aud_jack_quiet_info);
}

/*
 * Open a client without ever starting a server.
 *
 * JackNoStartServer is not a nicety: without it libjack spawns a daemon, and a
 * --list that quietly starts a sound server and takes the card off whatever had
 * it is not a listing. It also makes "is JACK running" a question with an
 * answer, which auto-selection needs.
 *
 * `quiet` suppresses the diagnostic, for the probe that only wants a yes or no.
 */
static inline jack_client_t *aud_jack_open_client(const char *name, int quiet)
{
  static pthread_once_t quiet_once = PTHREAD_ONCE_INIT;
  jack_status_t status = 0;
  jack_client_t *client;

  pthread_once(&quiet_once, aud_jack_quiet_call);

  client = jack_client_open(name, JackNoStartServer, &status);
  if (client == NULL && !quiet)
  {
    if ((status & JackServerFailed) != 0 || (status & JackServerError) != 0)
    {
      aud_error("jack: cannot connect to the server");
      aud_info("is jackd running? try --backend alsa to open the card directly");
    }
    else
    {
      aud_error("jack: cannot create the client '%s' (status 0x%x)", name,
                (unsigned)status);
    }
  }
  return client;
}

/*
 * A JACK port name is "client:port". Everything audiaki calls a device is the
 * client half, so these three split one and compare the halves.
 */

/* Length of the client half of `port_name`, or 0 if it has no colon. */
static inline size_t aud_jack_client_part(const char *port_name)
{
  const char *colon = port_name != NULL ? strchr(port_name, ':') : NULL;

  return colon != NULL ? (size_t)(colon - port_name) : 0;
}

/* Whether `port_name` belongs to the client `name`. */
static inline int aud_jack_port_is_client(const char *port_name, const char *name)
{
  size_t len = aud_jack_client_part(port_name);

  return len > 0 && strlen(name) == len && strncmp(port_name, name, len) == 0;
}

/* Whether `port_name` belongs to one of audiaki's own clients. */
static inline int aud_jack_port_is_ours(const char *port_name)
{
  size_t prefix = strlen(JACK_CLIENT_PREFIX);
  size_t len = aud_jack_client_part(port_name);

  return len >= prefix && strncmp(port_name, JACK_CLIENT_PREFIX, prefix) == 0;
}

#endif /* AUDIAKI_JACK_COMMON_H */
