/* SPDX-License-Identifier: MIT */
/*
 * fonts.h - where the window gets its lettering from.
 *
 * One question, asked once at startup: is there a typeface on this machine
 * worth drawing in, and where is it. See fonts.c for what it looks at and why
 * finding nothing is not a failure.
 */
#ifndef AUDIAKI_GUI_FONTS_H
#define AUDIAKI_GUI_FONTS_H

#include <stddef.h>

typedef enum
{
  AUD_FONTS_SANS = 0,
  AUD_FONTS_STRONG,
  AUD_FONTS_MONO
} aud_fonts_kind;

/*
 * The path to the best `kind` on this machine, into `dst`. Returns non-zero
 * when there was one; zero leaves `dst` empty and means the caller should use
 * whatever font it already has.
 *
 * $AUDIAKI_FONT, $AUDIAKI_FONT_STRONG and $AUDIAKI_FONT_MONO are taken as
 * given without being looked for, for a machine whose fonts are somewhere this
 * does not know about or whose owner simply wants a different one.
 */
int aud_fonts_find(aud_fonts_kind kind, char *dst, size_t size);

#endif /* AUDIAKI_GUI_FONTS_H */
