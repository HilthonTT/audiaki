/* SPDX-License-Identifier: MIT */
#include "term/prompt.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int aud_prompt_available(void)
{
  return isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
}

int aud_prompt_line(const char *label, const char *fallback, char *dst, size_t size)
{
  char line[AUD_PROMPT_LINE_MAX];
  size_t len;

  if (dst == NULL || size == 0)
  {
    return -1;
  }
  if (label == NULL)
  {
    label = "";
  }
  if (fallback == NULL)
  {
    fallback = "";
  }

  if (*fallback != '\0')
  {
    fprintf(stderr, "%s [%s]: ", label, fallback);
  }
  else
  {
    fprintf(stderr, "%s: ", label);
  }
  fflush(stderr);

  /*
   * NULL covers end of input and the read being interrupted, which is what a
   * Ctrl+C at the question looks like: the handler sets the stop flag and the
   * read comes back rather than restarting. Both mean "stop asking me".
   */
  if (fgets(line, sizeof(line), stdin) == NULL)
  {
    fputc('\n', stderr);
    return -1;
  }

  len = strlen(line);
  if (len > 0 && line[len - 1] == '\n')
  {
    line[--len] = '\0';
  }
  else if (len + 1 == sizeof(line))
  {
    int c;

    /* the rest of an over-long line, so it is not read back as the next answer */
    while ((c = fgetc(stdin)) != '\n' && c != EOF)
    {
      ;
    }
    return -1;
  }

  /* trailing space is a copied path or a slipped thumb, never part of a name */
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
  {
    line[--len] = '\0';
  }

  {
    const char *answer = line;

    while (*answer == ' ' || *answer == '\t')
    {
      answer++;
    }
    if (*answer == '\0')
    {
      answer = fallback;
    }

    if (strlen(answer) >= size)
    {
      return -1;
    }
    snprintf(dst, size, "%s", answer);
  }

  return 0;
}
