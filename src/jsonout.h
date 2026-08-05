/* SPDX-License-Identifier: MIT */
/*
 * jsonout.h - just enough JSON to make --list, --probe and --info scriptable.
 *
 * Not a parser and not a document model. The printers that use this already
 * know the shape of what they are writing; the only thing they cannot do by
 * hand is put an arbitrary device name or file path inside quotes safely.
 *
 * Free of any ALSA or audio concern, so it can be unit tested anywhere.
 */
#ifndef AUDIAKI_JSONOUT_H
#define AUDIAKI_JSONOUT_H

#include <stdio.h>

/*
 * Write `s` as a quoted JSON string, escaping what the grammar requires:
 * backslash, double quote and everything below U+0020. Bytes above 0x7F pass
 * through untouched, so a UTF-8 device name stays UTF-8.
 *
 * A NULL string is written as the literal null, unquoted - which is what a
 * consumer wants for "this field has no value" rather than an empty string.
 */
void aud_json_string(FILE *out, const char *s);

/*
 * Write a number with `decimals` digits after the point.
 *
 * NaN and the infinities are written as null: JSON cannot spell them, and a
 * bare NaN token makes a strict parser reject the whole document rather than
 * the one field it could not read.
 */
void aud_json_number(FILE *out, double value, int decimals);

#endif /* AUDIAKI_JSONOUT_H */
