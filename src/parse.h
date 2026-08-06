/* SPDX-License-Identifier: MIT */
/*
 * parse.h - strict command line value parsing.
 *
 * strtoul() alone silently accepts "12abc" and "-1"; these helpers reject
 * anything that is not the whole, in-range value the flag asked for.
 */
#ifndef AUDIAKI_PARSE_H
#define AUDIAKI_PARSE_H

/*
 * Parse a decimal unsigned integer in [min, max]. Returns 0 on success and
 * -1 on trailing garbage, a negative sign, an empty string or a range miss.
 */
int parse_uint(const char *text, unsigned min, unsigned max, unsigned *out);

/*
 * Parse a non-negative decimal number in [min, max], e.g. "440" or "432.5".
 * Returns 0 on success, -1 on trailing garbage, a sign, an empty string, a
 * non-finite value or a range miss.
 */
int parse_double(const char *text, double min, double max, double *out);

/*
 * Parse a duration: "90", "12.5", "1:30" (mm:ss) or "1:02:03" (hh:mm:ss).
 * Returns 0 on success, -1 on malformed input or a negative result.
 */
int parse_duration(const char *text, double *out_seconds);

/*
 * Parse a frame size: "1280x720", or "720p"/"1080p"/"1440p"/"2160p" as
 * shorthand for the matching 16:9 size. Both dimensions must land in
 * [min, max]. Returns 0 on success, -1 on anything else.
 */
int parse_size(const char *text, unsigned min, unsigned max, unsigned *out_width,
               unsigned *out_height);

#endif /* AUDIAKI_PARSE_H */
