/* SPDX-License-Identifier: MIT */
#include "util/signals.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_stop = 1;
}

int aud_signals_install_stop(void)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGINT, &sa, NULL) != 0)
  {
    return -1;
  }
  if (sigaction(SIGTERM, &sa, NULL) != 0)
  {
    return -1;
  }
  return 0;
}

int aud_signals_stop_requested(void)
{
  return g_stop != 0;
}
