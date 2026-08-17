/* SPDX-License-Identifier: MIT */
#include "edit/project.h"

#include "edit/load.h"
#include "util/log.h"
#include "util/path.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A project is a list of clips, so it is short whatever the session holds. Past
 * this it is not one, and reading it into memory to find that out is worse than
 * saying so - the same judgement util/config.c makes about a config file.
 */
#define PROJECT_MAX_BYTES (4u * 1024u * 1024u)

/*
 * Room for a name that needs one. `why` is a static string everywhere else,
 * and for the failures that name a file it points here instead. Single
 * threaded, like everything else that touches a document - see player.h.
 */
static char project_detail[AUD_PATH_MAX + 96u];

static void say(const char **why, const char *text)
{
  if (why != NULL)
  {
    *why = text;
  }
}

/* The same, for the failures worth naming a file in. */
static void say_detail(const char **why, const char *fmt, ...) AUD_PRINTF(2, 3);

static void say_detail(const char **why, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(project_detail, sizeof(project_detail), fmt, ap);
  va_end(ap);
  say(why, project_detail);
}

int aud_project_is_project(const char *path)
{
  size_t len;
  size_t ext = sizeof(AUD_PROJECT_EXT) - 1u;

  if (path == NULL)
  {
    return 0;
  }

  len = strlen(path);
  if (len <= ext)
  {
    return 0;
  }

  for (size_t i = 0; i < ext; i++)
  {
    char a = path[len - ext + i];
    char b = AUD_PROJECT_EXT[i];

    if (a >= 'A' && a <= 'Z')
    {
      a = (char)(a - 'A' + 'a');
    }
    if (a != b)
    {
      return 0;
    }
  }
  return 1;
}

/* -- writing --------------------------------------------------------------- */

/*
 * The distinct blocks a project refers to, in the order they are written. Two
 * clips over one block - what a split leaves behind - name the same source, so
 * loading rebuilds the sharing rather than reading the file twice.
 */
typedef struct
{
  const aud_samples *block[AUD_PROJECT_MAX_SOURCES];
  size_t count;
} source_table;

/* Where `block` sits in the table, adding it if it is not there yet, or -1. */
static long source_index(source_table *st, const aud_samples *block)
{
  for (size_t i = 0; i < st->count; i++)
  {
    if (st->block[i] == block)
    {
      return (long)i;
    }
  }

  if (st->count == AUD_PROJECT_MAX_SOURCES)
  {
    return -1;
  }

  st->block[st->count] = block;
  return (long)st->count++;
}

/*
 * The format gives each value the rest of its line, so a value carrying a
 * newline of its own would end that line early and the reader would take what
 * followed for the next setting: a track called "take\nchannels 7" writes a
 * channel count the reader believes. A filename may hold one - the filesystem
 * allows it - and so may anything typed into a name field.
 *
 * Non-zero when `text` would stay on the line it was written to.
 */
static int one_line(const char *text)
{
  return strcspn(text, "\r\n") == strlen(text);
}

/*
 * A name reduced to something that will. Cosmetic, which is why this trims
 * rather than refusing: the label on a lane is not worth failing a save over,
 * and the audio and its placement come back either way. A source path is not
 * cosmetic and is refused instead - see collect_sources().
 */
static void tidy_name(char *dst, size_t size, const char *name)
{
  size_t n = 0;

  for (const char *p = name; *p != '\0' && n + 1u < size; p++)
  {
    unsigned char c = (unsigned char)*p;

    dst[n++] = (c == '\n' || c == '\r') ? ' ' : (char)c;
  }
  dst[n] = '\0';
}

/* Walk every clip and collect the blocks. Returns 0, or -1 with `*why` set. */
static int collect_sources(const aud_doc *d, source_table *st, const char **why)
{
  st->count = 0;

  for (size_t i = 0; i < d->count; i++)
  {
    const aud_track *t = &d->tracks[i];

    for (size_t c = 0; c < t->count; c++)
    {
      const aud_samples *block = t->clips[c].audio;

      if (block == NULL)
      {
        continue;
      }
      if (aud_samples_source(block)[0] == '\0')
      {
        say_detail(why, "'%s' holds audio that is not saved anywhere yet", t->name);
        return -1;
      }
      /*
       * Refused rather than trimmed, because a trimmed path names a different
       * file or none at all - and a project that opens pointing at nothing has
       * lost the take as surely as deleting it would have. A name is only a
       * label and is trimmed instead; see tidy_name().
       */
      if (!one_line(aud_samples_source(block)))
      {
        char shown[AUD_TRACK_NAME_MAX];

        /* through the same trim, so the complaint about a line break is not
         * itself spread over two lines of somebody's status bar */
        tidy_name(shown, sizeof(shown), aud_path_basename(aud_samples_source(block)));
        say_detail(why,
                   "'%s' has a line break in its name, which a project file cannot "
                   "refer to - rename it and save again",
                   shown);
        return -1;
      }
      if (source_index(st, block) < 0)
      {
        say(why, "that project refers to more files than one can hold");
        return -1;
      }
    }
  }
  return 0;
}

static int write_sources(FILE *f, const source_table *st, const char *dir)
{
  for (size_t i = 0; i < st->count; i++)
  {
    char stored[AUD_PATH_MAX];
    const char *source = aud_samples_source(st->block[i]);

    /* relative where it can be, so a session folder can be moved as a unit */
    if (aud_path_relative(stored, sizeof(stored), dir, source) != 0)
    {
      return -1;
    }
    if (fprintf(f, "source %s\n", stored) < 0)
    {
      return -1;
    }
  }
  return 0;
}

static int write_track(FILE *f, const aud_track *t, const source_table *st)
{
  char name[AUD_TRACK_NAME_MAX];

  tidy_name(name, sizeof(name), t->name);
  if (fprintf(f, "track\nname %s\nchannels %u\n", name, t->channels) < 0)
  {
    return -1;
  }
  if (fprintf(f, "gain %.6f\npan %.6f\n", (double)t->gain, (double)t->pan) < 0)
  {
    return -1;
  }
  if (fprintf(f, "muted %d\nsoloed %d\ncollapsed %d\nheight %d\n", t->muted ? 1 : 0,
              t->soloed ? 1 : 0, t->collapsed ? 1 : 0, t->height) < 0)
  {
    return -1;
  }

  for (size_t c = 0; c < t->count; c++)
  {
    const aud_clip *clip = &t->clips[c];
    long index = -1;

    for (size_t i = 0; i < st->count; i++)
    {
      if (st->block[i] == clip->audio)
      {
        index = (long)i;
        break;
      }
    }
    if (index < 0)
    {
      return -1;
    }

    if (fprintf(f, "clip %ld %zu %zu %llu %zu %zu\n", index, clip->offset, clip->frames,
                (unsigned long long)clip->start, clip->fade_in, clip->fade_out) < 0)
    {
      return -1;
    }
  }
  return 0;
}

int aud_project_save(const aud_doc *d, const char *path, const char **why)
{
  source_table st;
  char dir[AUD_PATH_MAX];
  char partial[AUD_PATH_MAX];
  FILE *f;

  say(why, NULL);

  if (d == NULL || path == NULL || *path == '\0')
  {
    say(why, "there is nowhere to save that");
    return -1;
  }

  if (aud_path_dirname(dir, sizeof(dir), path) != 0 ||
      (size_t)snprintf(partial, sizeof(partial), "%s.saving", path) >= sizeof(partial))
  {
    say(why, "that is too long a path");
    return -1;
  }

  if (collect_sources(d, &st, why) != 0)
  {
    return -1;
  }

  /*
   * Written beside the destination and renamed over it, so a save that fails
   * part way leaves the version that was already there rather than half of a
   * new one. The same rule gui/render.c follows for a video.
   */
  f = fopen(partial, "wb");
  if (f == NULL)
  {
    say_detail(why, "cannot write there: %s", strerror(errno));
    return -1;
  }

  if (fprintf(f, "%s %d\n", AUD_PROJECT_MAGIC, AUD_PROJECT_VERSION) < 0 ||
      fprintf(f, "rate %u\n", d->rate) < 0 ||
      fprintf(f, "tempo %.6f %u\n", d->tempo, d->beats_per_bar) < 0 ||
      fprintf(f, "grid %u\n", d->grid_div) < 0 ||
      fprintf(f, "cursor %llu\n", (unsigned long long)d->cursor) < 0 ||
      fprintf(f, "selection %llu %llu\n", (unsigned long long)d->sel_start,
              (unsigned long long)d->sel_end) < 0 ||
      write_sources(f, &st, dir) != 0)
  {
    goto failed;
  }

  for (size_t i = 0; i < d->count; i++)
  {
    if (write_track(f, &d->tracks[i], &st) != 0)
    {
      goto failed;
    }
  }

  if (fclose(f) != 0)
  {
    remove(partial);
    say(why, "the project could not be written");
    return -1;
  }

  if (rename(partial, path) != 0)
  {
    remove(partial);
    say_detail(why, "cannot save there: %s", strerror(errno));
    return -1;
  }
  return 0;

failed:
  fclose(f);
  remove(partial);
  say(why, "the project could not be written");
  return -1;
}

/* -- reading --------------------------------------------------------------- */

/* Split `line` into its keyword and whatever follows it, both trimmed. */
static char *split_word(char *line, char **rest)
{
  char *at = line;
  char *word;

  while (*at == ' ' || *at == '\t')
  {
    at++;
  }
  word = at;

  while (*at != '\0' && *at != ' ' && *at != '\t')
  {
    at++;
  }
  if (*at != '\0')
  {
    *at++ = '\0';
    while (*at == ' ' || *at == '\t')
    {
      at++;
    }
  }

  *rest = at;
  return word;
}

/* An unsigned decimal, advancing `*at` past it. Returns 0 on success. */
static int take_u64(char **at, uint64_t *out)
{
  char *end = NULL;
  unsigned long long value;

  while (**at == ' ' || **at == '\t')
  {
    (*at)++;
  }
  if (**at < '0' || **at > '9')
  {
    return -1;
  }

  errno = 0;
  value = strtoull(*at, &end, 10);
  if (errno != 0 || end == *at)
  {
    return -1;
  }

  *at = end;
  *out = (uint64_t)value;
  return 0;
}

static float clampf(float v, float lo, float hi)
{
  if (!(v >= lo))
  {
    return lo; /* also catches a NaN out of strtod */
  }
  return v > hi ? hi : v;
}

static int take_double(const char *text, double *out)
{
  char *end = NULL;
  double value;

  errno = 0;
  value = strtod(text, &end);
  if (errno != 0 || end == text || !(value >= -1e6 && value <= 1e6))
  {
    return -1;
  }

  *out = value;
  return 0;
}

static int take_float(const char *text, float *out)
{
  double value;

  if (take_double(text, &value) != 0)
  {
    return -1;
  }

  *out = (float)value;
  return 0;
}

static int take_int(const char *text, int *out, int min, int max)
{
  char *end = NULL;
  long value;

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || value < (long)min || value > (long)max)
  {
    return -1;
  }

  *out = (int)value;
  return 0;
}

typedef struct
{
  char path[AUD_PROJECT_MAX_SOURCES][AUD_PATH_MAX];
  aud_samples *block[AUD_PROJECT_MAX_SOURCES];
  size_t count;
} loaded_sources;

static void loaded_free(loaded_sources *ls)
{
  for (size_t i = 0; i < ls->count; i++)
  {
    aud_samples_release(ls->block[i]);
    ls->block[i] = NULL;
  }
  ls->count = 0;
}

/*
 * Read source `index`, if it has not been read already. Resolved against the
 * project's own folder, so a relative reference means "beside the project".
 */
static aud_samples *source_block(loaded_sources *ls, size_t index, const char *dir,
                                 unsigned rate, const char **why)
{
  char full[AUD_PATH_MAX];
  const char *reason = NULL;
  unsigned found = 0;

  if (index >= ls->count)
  {
    say(why, "that project names a source it does not list");
    return NULL;
  }
  if (ls->block[index] != NULL)
  {
    return ls->block[index];
  }

  if (aud_path_join(full, sizeof(full), dir, ls->path[index]) != 0)
  {
    say(why, "that project refers to too long a path");
    return NULL;
  }

  ls->block[index] = aud_edit_read_wav(full, &found, &reason);
  if (ls->block[index] == NULL)
  {
    say_detail(why, "cannot open '%s' - has it been moved?", ls->path[index]);
    return NULL;
  }

  if (found != rate)
  {
    say_detail(why, "'%s' is at a different sample rate from the project",
               ls->path[index]);
    aud_samples_release(ls->block[index]);
    ls->block[index] = NULL;
    return NULL;
  }
  return ls->block[index];
}

/* One `clip` line: SOURCE OFFSET FRAMES START FADE_IN FADE_OUT. */
static int read_clip(aud_track *t, char *args, loaded_sources *ls, const char *dir,
                     unsigned rate, const char **why)
{
  uint64_t field[6];
  aud_samples *block;

  for (size_t i = 0; i < 6; i++)
  {
    if (take_u64(&args, &field[i]) != 0)
    {
      say(why, "a clip in that project is malformed");
      return -1;
    }
  }

  block = source_block(ls, (size_t)field[0], dir, rate, why);
  if (block == NULL)
  {
    return -1;
  }

  if (aud_track_place(t, block, (size_t)field[1], (size_t)field[2], field[3],
                      (size_t)field[4], (size_t)field[5]) != 0)
  {
    say(why, "a clip in that project does not fit the audio it names");
    return -1;
  }
  return 0;
}

static char *read_file(const char *path, const char **why)
{
  FILE *f = fopen(path, "rb");
  char *text;
  long size;
  size_t got;

  if (f == NULL)
  {
    say_detail(why, "cannot open that project: %s", strerror(errno));
    return NULL;
  }

  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    say(why, "that project could not be read");
    return NULL;
  }
  if ((unsigned long)size > PROJECT_MAX_BYTES)
  {
    fclose(f);
    say(why, "that file is too large to be a project");
    return NULL;
  }

  text = malloc((size_t)size + 1u);
  if (text == NULL)
  {
    fclose(f);
    say(why, "not enough memory to read that project");
    return NULL;
  }

  got = fread(text, 1, (size_t)size, f);
  text[got] = '\0';
  fclose(f);
  return text;
}

int aud_project_load(aud_doc *d, const char *path, const char **why)
{
  aud_doc built;
  loaded_sources ls;
  char dir[AUD_PATH_MAX];
  char *text;
  char *at;
  aud_track *track = NULL;
  unsigned rate = 0;
  int rc = -1;
  int seen_header = 0;

  say(why, NULL);

  if (d == NULL || path == NULL)
  {
    say(why, "there is no project to open");
    return -1;
  }

  if (aud_path_dirname(dir, sizeof(dir), path) != 0)
  {
    say(why, "that is too long a path");
    return -1;
  }

  text = read_file(path, why);
  if (text == NULL)
  {
    return -1;
  }

  memset(&ls, 0, sizeof(ls));
  aud_doc_init(&built, 0);
  at = text;

  while (*at != '\0')
  {
    char *line = at;
    char *end = strchr(line, '\n');
    char *word;
    char *args;

    if (end == NULL)
    {
      end = line + strlen(line);
      at = end;
    }
    else
    {
      *end = '\0';
      at = end + 1;
    }

    /* a CRLF file opens the same as a LF one, which someone will produce */
    if (end > line && end[-1] == '\r')
    {
      end[-1] = '\0';
    }

    word = split_word(line, &args);
    if (*word == '\0' || *word == '#')
    {
      continue;
    }

    if (!seen_header)
    {
      int version = 0;

      if (strcmp(word, AUD_PROJECT_MAGIC) != 0 ||
          take_int(args, &version, 1, AUD_PROJECT_VERSION) != 0)
      {
        say(why, "that is not a project audiaki can open");
        goto out;
      }
      seen_header = 1;
      continue;
    }

    if (strcmp(word, "rate") == 0)
    {
      int value = 0;

      if (take_int(args, &value, 1, 768000) != 0)
      {
        say(why, "that project has no usable sample rate");
        goto out;
      }
      rate = (unsigned)value;
      built.rate = rate;
      continue;
    }

    if (strcmp(word, "tempo") == 0)
    {
      double bpm = 0.0;
      uint64_t beats = AUD_CLICK_DEFAULT_BEATS;
      char *rest = args;

      /*
       * "tempo BPM BEATS", with the beats optional: a line typed by hand is
       * likely to say only the number anybody means by a tempo, and four to
       * the bar is what that means. Out-of-range values are clamped by
       * aud_doc_set_tempo() rather than refused - a project should still open
       * when someone has put 5000 in it.
       */
      if (take_double(args, &bpm) == 0)
      {
        while (*rest != '\0' && *rest != ' ' && *rest != '\t')
        {
          rest++;
        }
        (void)take_u64(&rest, &beats);
        aud_doc_set_tempo(&built, bpm, (unsigned)beats);
      }
      continue;
    }

    if (strcmp(word, "grid") == 0)
    {
      uint64_t div = AUD_DOC_GRID_BEAT;
      char *rest = args;

      /*
       * Absent in a project written before there was anything but beats to
       * snap to, and aud_doc_init() has already put beats there - so an old
       * project opens on the grid it was drawn with.
       */
      if (take_u64(&rest, &div) == 0)
      {
        aud_doc_set_grid(&built, (unsigned)div);
      }
      continue;
    }

    if (strcmp(word, "source") == 0)
    {
      if (ls.count == AUD_PROJECT_MAX_SOURCES ||
          (size_t)snprintf(ls.path[ls.count], AUD_PATH_MAX, "%s", args) >= AUD_PATH_MAX)
      {
        say(why, "that project refers to more files than one can hold");
        goto out;
      }
      ls.count++;
      continue;
    }

    if (strcmp(word, "cursor") == 0)
    {
      uint64_t value = 0;

      if (take_u64(&args, &value) == 0)
      {
        built.cursor = value;
      }
      continue;
    }

    if (strcmp(word, "selection") == 0)
    {
      uint64_t from = 0;
      uint64_t to = 0;

      if (take_u64(&args, &from) == 0 && take_u64(&args, &to) == 0 && to >= from)
      {
        built.sel_start = from;
        built.sel_end = to;
      }
      continue;
    }

    if (strcmp(word, "track") == 0)
    {
      track = aud_doc_add_track(&built, "Track", 1);
      if (track == NULL)
      {
        say(why, "that project has more tracks than one can hold");
        goto out;
      }
      continue;
    }

    /*
     * Everything below describes the track most recently opened.
     *
     * A setting with no track to apply to is stepped over, the same way an
     * unknown keyword is: it costs nothing, and a file written by a later
     * version may well carry one. A clip is not a setting - it is audio, and
     * audio this cannot place has to be refused rather than quietly left out,
     * or a project whose track lines were damaged opens as a session missing
     * takes and says nothing went wrong. A file of nothing but clips used to
     * open as an empty session, successfully.
     */
    if (track == NULL)
    {
      if (strcmp(word, "clip") == 0)
      {
        say(why, "that project places audio before any track to put it on");
        goto out;
      }
      continue;
    }

    if (strcmp(word, "name") == 0)
    {
      snprintf(track->name, sizeof(track->name), "%s", args);
    }
    else if (strcmp(word, "channels") == 0)
    {
      int value = 0;

      if (take_int(args, &value, 1, 64) != 0)
      {
        say(why, "a track in that project has an impossible channel count");
        goto out;
      }
      track->channels = (unsigned)value;
    }
    else if (strcmp(word, "gain") == 0)
    {
      /* held to the range the fader offers, so a hand-edited file cannot ask
       * for a level no control in the window could take back */
      if (take_float(args, &track->gain) == 0)
      {
        track->gain = clampf(track->gain, 0.0f, 2.0f);
      }
    }
    else if (strcmp(word, "pan") == 0)
    {
      if (take_float(args, &track->pan) == 0)
      {
        track->pan = clampf(track->pan, -1.0f, 1.0f);
      }
    }
    else if (strcmp(word, "muted") == 0)
    {
      take_int(args, &track->muted, 0, 1);
    }
    else if (strcmp(word, "soloed") == 0)
    {
      take_int(args, &track->soloed, 0, 1);
    }
    else if (strcmp(word, "collapsed") == 0)
    {
      take_int(args, &track->collapsed, 0, 1);
    }
    else if (strcmp(word, "height") == 0)
    {
      take_int(args, &track->height, AUD_TRACK_HEIGHT_MIN, AUD_TRACK_HEIGHT_MAX);
    }
    else if (strcmp(word, "clip") == 0)
    {
      if (rate == 0)
      {
        say(why, "that project places audio before saying what rate it is at");
        goto out;
      }
      if (read_clip(track, args, &ls, dir, rate, why) != 0)
      {
        goto out;
      }
    }
    /* anything else is from a later version of the format; step over it */
  }

  if (!seen_header)
  {
    say(why, "that is not a project audiaki can open");
    goto out;
  }
  if (rate == 0)
  {
    say(why, "that project has no usable sample rate");
    goto out;
  }

  /*
   * Only now, with every source found and every clip placed: a load that fell
   * over half way leaves whatever was open untouched rather than replacing it
   * with the part that worked.
   */
  aud_doc_free(d);
  *d = built;
  memset(&built, 0, sizeof(built));
  rc = 0;

out:
  aud_doc_free(&built);
  loaded_free(&ls);
  free(text);
  return rc;
}
