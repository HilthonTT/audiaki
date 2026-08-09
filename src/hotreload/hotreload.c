/* SPDX-License-Identifier: MIT */
/*
 * hotreload.c - loading, and reloading, the app's library.
 *
 * Only built into the development window; see hotreload.h for why there is
 * nothing of this in a release binary.
 */
#include "hotreload/hotreload.h"

#ifndef AUDIAKI_HOTRELOAD

/*
 * A release build calls the app directly and the Makefile leaves this file out
 * of it entirely. The typedef is here so that a tool which compiles the tree
 * without asking - an editor's index, a static analyser - still meets a legal
 * translation unit rather than an empty one.
 */
typedef int aud_hotreload_not_built;

#else

#include "util/log.h"
#include "util/path.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What the Makefile writes beside the binary. */
#define AUD_PLUG_LIB "libaudiaki-gui.so"

#define PLUG(name, ...) aud_plug_##name##_t *aud_hot_##name = NULL;
AUD_LIST_OF_PLUGS
#undef PLUG

/*
 * Everything the shell calls, resolved as one: a library that has lost one of
 * these - a file left out of the link, a name that was renamed halfway - must
 * not replace a library that has all of them. So they are found into here
 * first and only then swapped in.
 */
typedef struct
{
#define PLUG(name, ...) aud_plug_##name##_t *name;
  AUD_LIST_OF_PLUGS
#undef PLUG
} plug_table;

static void *plug_lib;

/* Where the library is: $AUDIAKI_GUI_PLUG, or beside the binary. */
static int plug_path(char *dst, size_t size)
{
  const char *env = getenv("AUDIAKI_GUI_PLUG");
  char exe[AUD_PATH_MAX];
  char *slash;
  ssize_t len;

  if (env != NULL && env[0] != '\0')
  {
    return (size_t)snprintf(dst, size, "%s", env) < size ? 0 : -1;
  }

  /*
   * Beside the binary rather than through a search path or the working
   * directory: the two are built together and belong together, and a window
   * started from somewhere else should not find yesterday's library.
   */
  len = readlink("/proc/self/exe", exe, sizeof(exe) - 1u);
  if (len <= 0)
  {
    return -1;
  }
  exe[len] = '\0';

  slash = strrchr(exe, '/');
  if (slash == NULL)
  {
    return -1;
  }
  *slash = '\0';

  return aud_path_join(dst, size, exe, AUD_PLUG_LIB);
}

static int plug_copy(const char *from, const char *to)
{
  char buf[32u * 1024u];
  FILE *in;
  FILE *out;
  size_t got;

  in = fopen(from, "rb");
  if (in == NULL)
  {
    return -1;
  }

  out = fopen(to, "wb");
  if (out == NULL)
  {
    fclose(in);
    return -1;
  }

  while ((got = fread(buf, 1u, sizeof(buf), in)) > 0)
  {
    if (fwrite(buf, 1u, got, out) != got)
    {
      break;
    }
  }

  if (ferror(in) || ferror(out) || fclose(out) != 0)
  {
    fclose(in);
    unlink(to);
    return -1;
  }

  fclose(in);
  return 0;
}

bool aud_hotreload_load(void)
{
  static unsigned generation;
  char path[AUD_PATH_MAX];
  char staged[AUD_PATH_MAX];
  plug_table fresh;
  void *lib;

  if (plug_path(path, sizeof(path)) != 0)
  {
    aud_error("cannot work out where " AUD_PLUG_LIB " is");
    return false;
  }

  /*
   * Loaded from a copy, under a name never used before.
   *
   * dlopen() keys on the name it was given, and the linker writes the rebuilt
   * library back over the same path, so opening that path again would hand
   * back the code that is already mapped about as often as it would load the
   * new one - a reload that silently did nothing, which is worse than one that
   * fails. The copy is unlinked the moment it is mapped: the mapping holds the
   * inode open, and a window that is killed leaves nothing behind in build/.
   */
  if ((size_t)snprintf(staged, sizeof(staged), "%s.%ld.%u", path, (long)getpid(),
                       ++generation) >= sizeof(staged))
  {
    aud_error("cannot stage %s: the name is too long", path);
    return false;
  }

  if (plug_copy(path, staged) != 0)
  {
    aud_perror("cannot stage %s", path);
    return false;
  }

  lib = dlopen(staged, RTLD_NOW);
  unlink(staged);
  if (lib == NULL)
  {
    aud_error("cannot load %s: %s", path, dlerror());
    return false;
  }

  /*
   * The cast through void ** rather than a plain assignment: converting a data
   * pointer to a function pointer is not something ISO C has an opinion in
   * favour of, and -Wpedantic says so. POSIX guarantees the conversion; this
   * is how POSIX itself spells it.
   */
#define PLUG(name, ...)                                                     \
  *(void **)&fresh.name = dlsym(lib, "aud_plug_" #name);                    \
  if (fresh.name == NULL)                                                   \
  {                                                                         \
    aud_error("%s has no aud_plug_" #name ": %s", AUD_PLUG_LIB, dlerror()); \
    dlclose(lib);                                                           \
    return false;                                                           \
  }
  AUD_LIST_OF_PLUGS
#undef PLUG

#define PLUG(name, ...) aud_hot_##name = fresh.name;
  AUD_LIST_OF_PLUGS
#undef PLUG

  /* the old one only now, so nothing above can fail with it already gone */
  if (plug_lib != NULL)
  {
    dlclose(plug_lib);
  }
  plug_lib = lib;
  return true;
}

#endif /* AUDIAKI_HOTRELOAD */
