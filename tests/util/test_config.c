/* SPDX-License-Identifier: MIT */
#include "test_util.h"

#include "util/config.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST(an_empty_file_leaves_the_defaults)
{
  aud_config cfg;

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "", "test"), 0);
  CHECK(cfg.take_dir[0] == '\0');
  CHECK_EQ_INT(cfg.prompt, AUD_PROMPT_AUTO);
}

TEST(settings_are_read_around_their_space_and_comments)
{
  aud_config cfg;
  const char *text = "# where takes go\n"
                     "\n"
                     "  take_dir  =  /takes/here  \n"
                     "; another comment style\n"
                     "prompt=no\n";

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, text, "test"), 0);
  CHECK(strcmp(cfg.take_dir, "/takes/here") == 0);
  CHECK_EQ_INT(cfg.prompt, AUD_PROMPT_NEVER);
}

TEST(take_dir_arrives_expanded)
{
  aud_config cfg;

  setenv("HOME", "/home/tester", 1);
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "take_dir = ~/Takes\n", "test"), 0);
  CHECK(strcmp(cfg.take_dir, "/home/tester/Takes") == 0);
}

TEST(an_empty_take_dir_means_here)
{
  aud_config cfg;

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "take_dir = /elsewhere\ntake_dir =\n", "test"), 0);
  CHECK(cfg.take_dir[0] == '\0');
}

TEST(prompt_takes_the_spellings_people_reach_for)
{
  aud_prompt_mode mode = AUD_PROMPT_AUTO;

  CHECK_EQ_INT(aud_config_prompt_parse("yes", &mode), 0);
  CHECK_EQ_INT(mode, AUD_PROMPT_ALWAYS);
  CHECK_EQ_INT(aud_config_prompt_parse("true", &mode), 0);
  CHECK_EQ_INT(mode, AUD_PROMPT_ALWAYS);
  CHECK_EQ_INT(aud_config_prompt_parse("off", &mode), 0);
  CHECK_EQ_INT(mode, AUD_PROMPT_NEVER);
  CHECK_EQ_INT(aud_config_prompt_parse("auto", &mode), 0);
  CHECK_EQ_INT(mode, AUD_PROMPT_AUTO);

  CHECK_EQ_INT(aud_config_prompt_parse("maybe", &mode), -1);
  CHECK_EQ_INT(mode, AUD_PROMPT_AUTO); /* unchanged by a value it rejected */

  CHECK(strcmp(aud_config_prompt_name(AUD_PROMPT_ALWAYS), "yes") == 0);
  CHECK(strcmp(aud_config_prompt_name(AUD_PROMPT_NEVER), "no") == 0);
  CHECK(strcmp(aud_config_prompt_name(AUD_PROMPT_AUTO), "auto") == 0);
}

TEST(latency_is_a_number_of_milliseconds_or_nothing_at_all)
{
  aud_config cfg;

  aud_config_defaults(&cfg);
  CHECK(cfg.latency_ms < 0.0); /* nothing said means work it out */

  CHECK_EQ_INT(aud_config_parse(&cfg, "latency_ms = 12.5\n", "t"), 0);
  CHECK_EQ_DBL(cfg.latency_ms, 12.5, 1e-9);

  /* the hyphenated spelling, as take-dir has */
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "latency-ms = 3\n", "t"), 0);
  CHECK_EQ_DBL(cfg.latency_ms, 3.0, 1e-9);

  /* nonsense is counted and put back to "work it out", not left half read */
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "latency_ms = soon\n", "t"), 1);
  CHECK(cfg.latency_ms < 0.0);

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "latency_ms = 99999\n", "t"), 1);
  CHECK(cfg.latency_ms < 0.0);
}

TEST(gain_is_a_multiplier_or_nothing_at_all)
{
  aud_config cfg;

  aud_config_defaults(&cfg);
  /* nothing said means the take is the samples the device delivered */
  CHECK(cfg.input_gain < 0.0);

  CHECK_EQ_INT(aud_config_parse(&cfg, "gain = 2.5\n", "t"), 0);
  CHECK_EQ_DBL(cfg.input_gain, 2.5, 1e-9);

  /* both of the longer spellings, for anyone who wrote what they meant */
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "input_gain = 4\n", "t"), 0);
  CHECK_EQ_DBL(cfg.input_gain, 4.0, 1e-9);

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "input-gain = 1.5\n", "t"), 0);
  CHECK_EQ_DBL(cfg.input_gain, 1.5, 1e-9);

  /* nonsense is counted and put back to "nothing said", not left half read */
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "gain = loud\n", "t"), 1);
  CHECK(cfg.input_gain < 0.0);

  /* and so is a multiplier past what an input trim is for */
  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "gain = 500\n", "t"), 1);
  CHECK(cfg.input_gain < 0.0);

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "gain = -2\n", "t"), 1);
  CHECK(cfg.input_gain < 0.0);
}

TEST(a_bad_line_is_counted_and_the_rest_is_still_read)
{
  aud_config cfg;
  const char *text = "nonsense\n"
                     "bogus = 1\n"
                     "prompt = maybe\n"
                     "take_dir = /takes\n";

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, text, "test"), 3);

  /* the point of counting rather than stopping: the good line still applied */
  CHECK(strcmp(cfg.take_dir, "/takes") == 0);
  CHECK_EQ_INT(cfg.prompt, AUD_PROMPT_AUTO);
}

TEST(a_file_with_no_last_newline_still_has_a_last_line)
{
  aud_config cfg;

  aud_config_defaults(&cfg);
  CHECK_EQ_INT(aud_config_parse(&cfg, "prompt = yes", "test"), 0);
  CHECK_EQ_INT(cfg.prompt, AUD_PROMPT_ALWAYS);
}

TEST(the_path_follows_the_environment)
{
  char path[AUD_PATH_MAX];

  setenv("HOME", "/home/tester", 1);
  unsetenv("AUDIAKI_CONFIG");
  unsetenv("XDG_CONFIG_HOME");

  CHECK_EQ_INT(aud_config_path(path, sizeof(path)), 0);
  CHECK(strcmp(path, "/home/tester/.config/audiaki/config") == 0);

  setenv("XDG_CONFIG_HOME", "/elsewhere", 1);
  CHECK_EQ_INT(aud_config_path(path, sizeof(path)), 0);
  CHECK(strcmp(path, "/elsewhere/audiaki/config") == 0);

  /* an explicit file is a file, not a folder to look inside */
  setenv("AUDIAKI_CONFIG", "~/audiaki.conf", 1);
  CHECK_EQ_INT(aud_config_path(path, sizeof(path)), 0);
  CHECK(strcmp(path, "/home/tester/audiaki.conf") == 0);

  unsetenv("AUDIAKI_CONFIG");
  unsetenv("XDG_CONFIG_HOME");
}

TEST(a_file_that_is_not_there_is_not_a_failure)
{
  aud_config cfg;

  setenv("AUDIAKI_CONFIG", "./no-such-audiaki-config", 1);
  CHECK_EQ_INT(aud_config_load(&cfg), 0);
  CHECK(cfg.take_dir[0] == '\0');
  CHECK_EQ_INT(cfg.prompt, AUD_PROMPT_AUTO);
  unsetenv("AUDIAKI_CONFIG");
}

int main(void)
{
  /* the parser reports what it could not use through log.h; the tests that
   * feed it rubbish do so on purpose, and the warnings are not the result */
  aud_log_set_level(AUD_LOG_QUIET);

  RUN(an_empty_file_leaves_the_defaults);
  RUN(settings_are_read_around_their_space_and_comments);
  RUN(take_dir_arrives_expanded);
  RUN(an_empty_take_dir_means_here);
  RUN(prompt_takes_the_spellings_people_reach_for);
  RUN(latency_is_a_number_of_milliseconds_or_nothing_at_all);
  RUN(gain_is_a_multiplier_or_nothing_at_all);
  RUN(a_bad_line_is_counted_and_the_rest_is_still_read);
  RUN(a_file_with_no_last_newline_still_has_a_last_line);
  RUN(the_path_follows_the_environment);
  RUN(a_file_that_is_not_there_is_not_a_failure);

  return TEST_RESULT();
}
