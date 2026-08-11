/* SPDX-License-Identifier: MIT */
#include "util/config.h"

#include "audio/format.h"
#include "take/latency.h"
#include "util/log.h"
#include "util/parse.h"
#include "util/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The size limit lives in the header, where a caller sizing a buffer can see it. */
#define CONFIG_MAX_BYTES AUD_CONFIG_MAX_BYTES

/* The longest key or value a line may carry, terminator included. */
#define CONFIG_FIELD_MAX AUD_PATH_MAX

static const char *skip_space(const char *p)
{
  while (*p == ' ' || *p == '\t')
  {
    p++;
  }
  return p;
}

/*
 * Copy [begin, end) into `dst` with the space at either end taken off.
 * Returns 0, or -1 when the field is longer than anything this understands.
 */
static int trimmed(char *dst, size_t size, const char *begin, const char *end)
{
  size_t len;

  begin = skip_space(begin);
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
  {
    end--;
  }

  len = (size_t)(end - begin);
  if (len >= size)
  {
    return -1;
  }

  memcpy(dst, begin, len);
  dst[len] = '\0';
  return 0;
}

void aud_config_defaults(aud_config *cfg)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->prompt = AUD_PROMPT_AUTO;
  cfg->latency_ms = -1.0; /* nothing said; work it out from the buffers */
  cfg->input_gain = -1.0; /* nothing said; the take is what the device sent */
}

const char *aud_config_prompt_name(aud_prompt_mode mode)
{
  switch (mode)
  {
  case AUD_PROMPT_NEVER:
    return "no";
  case AUD_PROMPT_ALWAYS:
    return "yes";
  case AUD_PROMPT_AUTO:
  default:
    return "auto";
  }
}

int aud_config_prompt_parse(const char *name, aud_prompt_mode *out)
{
  if (name == NULL || out == NULL)
  {
    return -1;
  }

  /*
   * The spellings anyone would reach for. A config file is written from
   * memory rather than from the manual, and rejecting "true" because the
   * example said "yes" is a poor way to greet someone.
   */
  if (strcmp(name, "auto") == 0)
  {
    *out = AUD_PROMPT_AUTO;
    return 0;
  }
  if (strcmp(name, "yes") == 0 || strcmp(name, "true") == 0 ||
      strcmp(name, "always") == 0 || strcmp(name, "on") == 0 || strcmp(name, "1") == 0)
  {
    *out = AUD_PROMPT_ALWAYS;
    return 0;
  }
  if (strcmp(name, "no") == 0 || strcmp(name, "false") == 0 ||
      strcmp(name, "never") == 0 || strcmp(name, "off") == 0 || strcmp(name, "0") == 0)
  {
    *out = AUD_PROMPT_NEVER;
    return 0;
  }

  return -1;
}

int aud_config_path(char *dst, size_t size)
{
  const char *explicit_path = getenv("AUDIAKI_CONFIG");
  const char *xdg = getenv("XDG_CONFIG_HOME");
  char base[AUD_PATH_MAX];
  char dir[AUD_PATH_MAX];

  if (dst == NULL || size == 0)
  {
    return -1;
  }

  /* an explicit file is a file, not a directory to look inside */
  if (explicit_path != NULL && *explicit_path != '\0')
  {
    return aud_path_expand(dst, size, explicit_path);
  }

  if (xdg != NULL && *xdg != '\0')
  {
    if (aud_path_expand(base, sizeof(base), xdg) != 0)
    {
      return -1;
    }
  }
  else if (aud_path_expand(base, sizeof(base), "~/.config") != 0)
  {
    return -1;
  }

  if (aud_path_join(dir, sizeof(dir), base, AUD_CONFIG_DIR) != 0)
  {
    return -1;
  }
  return aud_path_join(dst, size, dir, AUD_CONFIG_FILE);
}

int aud_config_parse(aud_config *cfg, const char *text, const char *source)
{
  const char *at = text;
  unsigned line_no = 0;
  int bad = 0;

  if (cfg == NULL || text == NULL)
  {
    return 0;
  }
  if (source == NULL)
  {
    source = "config";
  }

  while (*at != '\0')
  {
    const char *line = at;
    const char *end = strchr(line, '\n');
    const char *equals;
    char key[CONFIG_FIELD_MAX];
    char value[CONFIG_FIELD_MAX];

    if (end == NULL)
    {
      end = line + strlen(line);
      at = end;
    }
    else
    {
      at = end + 1;
    }
    line_no++;

    line = skip_space(line);
    if (line >= end || *line == '#' || *line == ';' || *line == '\r')
    {
      continue;
    }

    equals = memchr(line, '=', (size_t)(end - line));
    if (equals == NULL)
    {
      aud_warn("%s:%u: expected 'key = value'", source, line_no);
      bad++;
      continue;
    }

    if (trimmed(key, sizeof(key), line, equals) != 0 ||
        trimmed(value, sizeof(value), equals + 1, end) != 0)
    {
      aud_warn("%s:%u: that line is too long to be a setting", source, line_no);
      bad++;
      continue;
    }

    if (strcmp(key, "take_dir") == 0 || strcmp(key, "take-dir") == 0)
    {
      /*
       * Expanded here rather than where it is used, so everything downstream
       * of the config is holding a path the kernel would accept. A '~' that
       * reached open(2) would create a directory of that name in the working
       * directory, which is nobody's idea of a home folder.
       */
      if (*value == '\0')
      {
        cfg->take_dir[0] = '\0';
      }
      else if (aud_path_expand(cfg->take_dir, sizeof(cfg->take_dir), value) != 0)
      {
        aud_warn("%s:%u: cannot work out where '%s' is", source, line_no, value);
        bad++;
      }
      continue;
    }

    if (strcmp(key, "latency_ms") == 0 || strcmp(key, "latency-ms") == 0)
    {
      if (parse_double(value, 0.0, AUD_LATENCY_MAX_MS, &cfg->latency_ms) != 0)
      {
        aud_warn("%s:%u: latency_ms is milliseconds, 0 to %.0f, not '%s'", source,
                 line_no, AUD_LATENCY_MAX_MS, value);
        cfg->latency_ms = -1.0;
        bad++;
      }
      continue;
    }

    if (strcmp(key, "gain") == 0 || strcmp(key, "input_gain") == 0 ||
        strcmp(key, "input-gain") == 0)
    {
      if (parse_double(value, AUD_GAIN_MIN, AUD_GAIN_MAX, &cfg->input_gain) != 0)
      {
        aud_warn("%s:%u: gain is a multiplier, %.1f to %.1f, not '%s'", source, line_no,
                 AUD_GAIN_MIN, AUD_GAIN_MAX, value);
        cfg->input_gain = -1.0;
        bad++;
      }
      continue;
    }

    if (strcmp(key, "prompt") == 0)
    {
      if (aud_config_prompt_parse(value, &cfg->prompt) != 0)
      {
        aud_warn("%s:%u: prompt is auto, yes or no, not '%s'", source, line_no, value);
        bad++;
      }
      continue;
    }

    aud_warn("%s:%u: unknown setting '%s'", source, line_no, key);
    bad++;
  }

  return bad;
}

int aud_config_load(aud_config *cfg)
{
  char path[AUD_PATH_MAX];
  char *text;
  FILE *f;
  long size;
  size_t got;

  aud_config_defaults(cfg);

  if (cfg == NULL || aud_config_path(path, sizeof(path)) != 0)
  {
    return 0;
  }

  f = fopen(path, "rb");
  if (f == NULL)
  {
    /* not having one is the normal case, and it is what the defaults describe */
    aud_debug("no config file at %s", path);
    return 0;
  }

  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
  {
    aud_perror("cannot read %s", path);
    fclose(f);
    return -1;
  }

  if ((unsigned long)size > CONFIG_MAX_BYTES)
  {
    aud_error("%s is %ld bytes, which is not a config file", path, size);
    fclose(f);
    return -1;
  }

  text = malloc((size_t)size + 1);
  if (text == NULL)
  {
    aud_error("cannot read %s: out of memory", path);
    fclose(f);
    return -1;
  }

  got = fread(text, 1, (size_t)size, f);
  text[got] = '\0';
  fclose(f);

  aud_debug("reading %s", path);
  aud_config_parse(cfg, text, path);
  free(text);
  return 0;
}

/*
 * Whether two spellings name the same setting. '_' and '-' are interchangeable
 * throughout, because aud_config_parse() accepts both and a rewrite that only
 * recognised one would leave the other in place and append a second copy.
 */
static int key_matches(const char *a, const char *b)
{
  for (; *a != '\0' && *b != '\0'; a++, b++)
  {
    char ca = *a == '-' ? '_' : *a;
    char cb = *b == '-' ? '_' : *b;

    if (ca != cb)
    {
      return 0;
    }
  }
  return *a == *b;
}

/* Add `n` bytes to `dst`, keeping it terminated. Returns -1 when it will not fit. */
static int append(char *dst, size_t size, size_t *used, const char *text, size_t n)
{
  if (*used + n + 1 > size)
  {
    return -1;
  }

  memcpy(dst + *used, text, n);
  *used += n;
  dst[*used] = '\0';
  return 0;
}

int aud_config_set(char *dst, size_t size, const char *text, const char *key,
                   const char *value)
{
  const char *at = text != NULL ? text : "";
  size_t used = 0;
  int replaced = 0;

  if (dst == NULL || size == 0 || key == NULL || value == NULL)
  {
    return -1;
  }
  dst[0] = '\0';

  while (*at != '\0')
  {
    const char *line = at;
    const char *end = strchr(line, '\n');
    const char *stop;
    const char *body;
    const char *equals;
    char found[CONFIG_FIELD_MAX];
    int ours = 0;

    if (end == NULL)
    {
      stop = line + strlen(line);
      at = stop;
    }
    else
    {
      stop = end;
      at = end + 1;
    }

    body = skip_space(line);
    if (body < stop && *body != '#' && *body != ';')
    {
      equals = memchr(body, '=', (size_t)(stop - body));
      if (equals != NULL && trimmed(found, sizeof(found), body, equals) == 0 &&
          key_matches(found, key))
      {
        ours = 1;
      }
    }

    if (ours)
    {
      char line_text[CONFIG_FIELD_MAX];
      int n = snprintf(line_text, sizeof(line_text), "%s = %s\n", key, value);

      if (n < 0 || (size_t)n >= sizeof(line_text) ||
          append(dst, size, &used, line_text, (size_t)n) != 0)
      {
        return -1;
      }
      replaced = 1;
      continue;
    }

    /*
     * The line as it stands, and then a newline whether or not it had one. A
     * file whose last line was unterminated is left terminated, which is the
     * one change to somebody else's text worth making: the next append would
     * otherwise land on the end of their last setting.
     */
    if (append(dst, size, &used, line, (size_t)(stop - line)) != 0 ||
        append(dst, size, &used, "\n", 1) != 0)
    {
      return -1;
    }
  }

  if (!replaced)
  {
    char line_text[CONFIG_FIELD_MAX];
    int n = snprintf(line_text, sizeof(line_text), "%s = %s\n", key, value);

    if (n < 0 || (size_t)n >= sizeof(line_text) ||
        append(dst, size, &used, line_text, (size_t)n) != 0)
    {
      return -1;
    }
  }

  return 0;
}

/* Read the config file into a fresh buffer, or an empty one when there is none. */
static char *config_text(const char *path)
{
  char *text;
  FILE *f = fopen(path, "rb");
  long size;
  size_t got;

  if (f == NULL)
  {
    /* not having one yet is the ordinary case for the first thing written */
    text = malloc(1);
    if (text != NULL)
    {
      text[0] = '\0';
    }
    return text;
  }

  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
      (unsigned long)size > CONFIG_MAX_BYTES)
  {
    aud_error("cannot read %s", path);
    fclose(f);
    return NULL;
  }

  text = malloc((size_t)size + 1);
  if (text == NULL)
  {
    fclose(f);
    return NULL;
  }

  got = fread(text, 1, (size_t)size, f);
  text[got] = '\0';
  fclose(f);
  return text;
}

int aud_config_save(const char *key, const char *value, char *path, size_t path_size)
{
  char file[AUD_PATH_MAX];
  char temp[AUD_PATH_MAX];
  char dir[AUD_PATH_MAX];
  char *text;
  char *updated;
  FILE *out;
  int rc = -1;

  if (key == NULL || value == NULL)
  {
    return -1;
  }

  if (aud_config_path(file, sizeof(file)) != 0)
  {
    aud_error("there is no home directory to keep a config file in");
    return -1;
  }

  if (aud_path_dirname(dir, sizeof(dir), file) != 0 || aud_path_mkdirs(dir) != 0)
  {
    aud_perror("cannot create %s", dir);
    return -1;
  }

  if (snprintf(temp, sizeof(temp), "%s.new", file) >= (int)sizeof(temp))
  {
    aud_error("the path to %s is too long to write beside", file);
    return -1;
  }

  text = config_text(file);
  if (text == NULL)
  {
    return -1;
  }

  /* room for the file as it stands plus the line being added to it */
  updated = malloc(CONFIG_MAX_BYTES + CONFIG_FIELD_MAX);
  if (updated == NULL)
  {
    aud_error("cannot rewrite %s: out of memory", file);
    free(text);
    return -1;
  }

  if (aud_config_set(updated, CONFIG_MAX_BYTES + CONFIG_FIELD_MAX, text, key, value) != 0)
  {
    aud_error("%s is too long to add a setting to", file);
    goto out;
  }

  out = fopen(temp, "wb");
  if (out == NULL)
  {
    aud_perror("cannot write %s", temp);
    goto out;
  }

  if (fputs(updated, out) == EOF || fclose(out) != 0)
  {
    aud_perror("cannot write %s", temp);
    remove(temp);
    goto out;
  }

  /*
   * rename(2) rather than aud_path_move(), which is the one place in audiaki
   * that wants the behaviour path.h deliberately refuses: replacing what is
   * already there. A take must never land on an older take, but this file is
   * meant to be replaced, and doing it in one step is what keeps a config that
   * exists from ever being half written.
   */
  if (rename(temp, file) != 0)
  {
    aud_perror("cannot replace %s", file);
    remove(temp);
    goto out;
  }

  if (path != NULL && path_size > 0)
  {
    snprintf(path, path_size, "%s", file);
  }
  rc = 0;

out:
  free(updated);
  free(text);
  return rc;
}
