/* SPDX-License-Identifier: MIT */
/*
 * fonts.c - finding a typeface on the machine the window is running on.
 *
 * raylib carries one font, and it is a bitmap: fine at the size it was drawn
 * at, a staircase at any other, and unmistakably not what the rest of the
 * desktop is lettered in. Every other part of this window has been made to look
 * like the machine it is on - the device names come from PipeWire, the file
 * chooser is the desktop's own - and the lettering should be no different.
 *
 * Shipping a font would mean an asset to install and a licence to carry, for a
 * program that is otherwise two binaries and a manual page. So instead this
 * walks the places a system keeps its fonts and takes the best one it finds,
 * ranked by a table of families that are readable on a dark background at 13
 * pixels. Nothing found is not an error: ui.c falls back to raylib's own, and
 * the window looks the way it looked before this file existed.
 *
 * The walk is bounded - a handful of roots, three levels deep, filenames only,
 * no stat() - and runs once, the first time anything is drawn.
 */
#include "gui/fonts.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How far below a root a font is allowed to hide. Debian buries them two deep
 * (truetype/dejavu/DejaVuSans.ttf); nobody buries them three. */
#define FONTS_MAX_DEPTH 3

/*
 * What to look for, best first. The rank is the index, so a machine with Inter
 * on it is lettered in Inter and one with nothing but DejaVu is lettered in
 * DejaVu, without either having to be asked about.
 *
 * Only TrueType. stb_truetype, which is what raylib parses with, reads OpenType
 * outlines poorly and font collections not at all - so Cantarell (.otf) and
 * Helvetica (.ttc) are left out however good they would look.
 */
static const char *const fonts_sans[] = {
    "Inter-Regular.ttf",  "InterDisplay-Regular.ttf",
    "Ubuntu-R.ttf",       "NotoSans-Regular.ttf",
    "Roboto-Regular.ttf", "OpenSans-Regular.ttf",
    "DejaVuSans.ttf",     "LiberationSans-Regular.ttf",
    "Arimo-Regular.ttf",  "FreeSans.ttf",
    "segoeui.ttf",        "Arial.ttf",
    "Verdana.ttf",
};

static const char *const fonts_strong[] = {
    "Inter-SemiBold.ttf",
    "Inter-Medium.ttf",
    "InterDisplay-SemiBold.ttf",
    "Ubuntu-M.ttf",
    "Ubuntu-B.ttf",
    "NotoSans-SemiBold.ttf",
    "NotoSans-Medium.ttf",
    "Roboto-Medium.ttf",
    "OpenSans-SemiBold.ttf",
    "DejaVuSans-Bold.ttf",
    "LiberationSans-Bold.ttf",
    "Arimo-Bold.ttf",
    "FreeSansBold.ttf",
    "seguisb.ttf",
    "segoeuib.ttf",
    "Arial_Bold.ttf",
    "Arial-Bold.ttf",
};

static const char *const fonts_mono[] = {
    "JetBrainsMono-Regular.ttf",
    "RobotoMono-Regular.ttf",
    "SourceCodePro-Regular.ttf",
    "UbuntuMono-R.ttf",
    "NotoSansMono-Regular.ttf",
    "DejaVuSansMono.ttf",
    "LiberationMono-Regular.ttf",
    "Cousine-Regular.ttf",
    "FreeMono.ttf",
    "consola.ttf",
    "CourierNew.ttf",
};

#define FONTS_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Where a system keeps them. The user's own first: a font installed by hand is
 * a preference, and a preference outranks whatever the distribution shipped. */
static const char *const fonts_roots[] = {
    "~/.local/share/fonts",
    "~/.fonts",
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "/System/Library/Fonts/Supplemental",
    "/Library/Fonts",
    "/usr/X11R6/lib/X11/fonts",
    "C:\\Windows\\Fonts",
};

/* The best candidate seen so far while a walk is running. */
typedef struct
{
  const char *const *wanted;
  int rank; /* index into `wanted`, or its length for nothing yet */
  char path[512];
} fonts_hunt;

/* Case-insensitive, because Windows and more than one Linux packager disagree
 * about whether a font file starts with a capital. */
static int name_matches(const char *file, const char *want)
{
  size_t i;

  for (i = 0; file[i] != '\0' && want[i] != '\0'; i++)
  {
    int a = file[i];
    int b = want[i];

    if (a >= 'A' && a <= 'Z')
    {
      a += 'a' - 'A';
    }
    if (b >= 'A' && b <= 'Z')
    {
      b += 'a' - 'A';
    }
    if (a != b)
    {
      return 0;
    }
  }
  return file[i] == '\0' && want[i] == '\0';
}

static void consider(fonts_hunt *hunt, const char *dir, const char *file)
{
  for (int i = 0; i < hunt->rank; i++)
  {
    if (name_matches(file, hunt->wanted[i]))
    {
      snprintf(hunt->path, sizeof(hunt->path), "%s/%s", dir, file);
      hunt->rank = i;
      return;
    }
  }
}

static void walk(fonts_hunt *hunt, const char *dir, int depth)
{
  DIR *d;
  struct dirent *entry;

  if (depth > FONTS_MAX_DEPTH || hunt->rank == 0)
  {
    return; /* nothing left that could beat what is already held */
  }

  d = opendir(dir);
  if (d == NULL)
  {
    return;
  }

  while ((entry = readdir(d)) != NULL)
  {
    char child[512];

    if (entry->d_name[0] == '.')
    {
      continue;
    }

    /*
     * d_type is what makes this cheap enough to do at startup - a stat() per
     * file over a few thousand of them is a visible pause.
     */
#ifdef DT_DIR
    if (entry->d_type == DT_REG)
    {
      consider(hunt, dir, entry->d_name);
      continue;
    }
    if (entry->d_type != DT_DIR && entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN)
    {
      continue;
    }
    /* a link or an unnamed kind could be either, so it is tried as both and
     * one of the two is wrong and harmless */
    if (entry->d_type != DT_DIR)
    {
      consider(hunt, dir, entry->d_name);
    }
#else
    consider(hunt, dir, entry->d_name);
#endif

    if (snprintf(child, sizeof(child), "%s/%s", dir, entry->d_name) < (int)sizeof(child))
    {
      walk(hunt, child, depth + 1);
    }

    if (hunt->rank == 0)
    {
      break;
    }
  }

  closedir(d);
}

/* "~/..." spelled out, since opendir does not know what a tilde is. */
static int expand_root(char *dst, size_t size, const char *root)
{
  const char *home;

  if (root[0] != '~')
  {
    return snprintf(dst, size, "%s", root) < (int)size;
  }

  home = getenv("HOME");
  if (home == NULL || *home == '\0')
  {
    return 0;
  }
  return snprintf(dst, size, "%s%s", home, root + 1) < (int)size;
}

static int hunt_for(char *dst, size_t size, const char *const *wanted, int count)
{
  fonts_hunt hunt;

  hunt.wanted = wanted;
  hunt.rank = count;
  hunt.path[0] = '\0';

  for (int i = 0; i < FONTS_COUNT(fonts_roots); i++)
  {
    char root[512];

    if (!expand_root(root, sizeof(root), fonts_roots[i]))
    {
      continue;
    }
    walk(&hunt, root, 0);
    if (hunt.rank == 0)
    {
      break;
    }
  }

  if (hunt.rank >= count)
  {
    return 0;
  }

  snprintf(dst, size, "%s", hunt.path);
  return 1;
}

int aud_fonts_find(aud_fonts_kind kind, char *dst, size_t size)
{
  const char *env;

  if (dst == NULL || size == 0)
  {
    return 0;
  }
  dst[0] = '\0';

  /*
   * An escape hatch, mostly for the person who dislikes the choice this made.
   * Three variables rather than one so a mono face can be swapped without
   * having to name the other two.
   */
  switch (kind)
  {
  case AUD_FONTS_STRONG:
    env = getenv("AUDIAKI_FONT_STRONG");
    break;
  case AUD_FONTS_MONO:
    env = getenv("AUDIAKI_FONT_MONO");
    break;
  case AUD_FONTS_SANS:
  default:
    env = getenv("AUDIAKI_FONT");
    break;
  }

  if (env != NULL && *env != '\0')
  {
    snprintf(dst, size, "%s", env);
    return 1;
  }

  switch (kind)
  {
  case AUD_FONTS_STRONG:
    return hunt_for(dst, size, fonts_strong, FONTS_COUNT(fonts_strong));
  case AUD_FONTS_MONO:
    return hunt_for(dst, size, fonts_mono, FONTS_COUNT(fonts_mono));
  case AUD_FONTS_SANS:
  default:
    return hunt_for(dst, size, fonts_sans, FONTS_COUNT(fonts_sans));
  }
}
