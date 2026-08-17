/* SPDX-License-Identifier: MIT */
/*
 * Turning argv into a request.
 *
 * cli.c is nine hundred lines of option table and cross-checks, and for a long
 * while the only thing that exercised any of it was three smoke assertions in
 * CI. That is the wrong way round: every one of these rules exists because
 * accepting the invocation would have done something quietly other than what
 * was asked - --json on a command with nothing to serialise, --dir on one that
 * writes no take, --bits on one that writes no mix - and a rule that stops
 * firing does not crash, it silently lets the thing through.
 *
 * It is testable because cli.h has always kept an audio system out of the
 * option handling: what comes out is an aud_options, and nothing here opens a
 * device. The only thing it reaches outside the parser is the backend name
 * table, and the ops that table points at are stubbed below - naming a backend
 * and binding one are different questions, and only the first is asked here.
 *
 * Both what is accepted and what is refused are checked, and the refusals are
 * checked by their reason: an invocation rejected for the wrong reason is a
 * message that sends somebody looking in the wrong place.
 */
#include "test_util.h"

#include "cli/cli.h"

#include "backend/backend.h"
#include "backend/device.h"
#include "util/log.h"
#include "util/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * What backend.c's table points at. Nothing here opens anything, so an ops
 * struct with no operations in it is exactly right: it says "this build has
 * that backend", which is the whole of what --backend and the help text ask.
 * The daemon probes answer no, which is the truthful answer under a test.
 */
#ifdef AUDIAKI_HAVE_ALSA
const aud_capture_ops aud_capture_ops_alsa = {"alsa", NULL, NULL, NULL, NULL,
                                              NULL,   NULL, NULL, NULL, NULL};
const aud_monitor_ops aud_monitor_ops_alsa = {"alsa", NULL, NULL, NULL,
                                              NULL,   NULL, NULL, NULL};
#endif

#ifdef AUDIAKI_HAVE_PIPEWIRE
const aud_capture_ops aud_capture_ops_pipewire = {"pipewire", NULL, NULL, NULL, NULL,
                                                  NULL,       NULL, NULL, NULL, NULL};
const aud_monitor_ops aud_monitor_ops_pipewire = {"pipewire", NULL, NULL, NULL,
                                                  NULL,       NULL, NULL, NULL};
int aud_pipewire_daemon_responds(void)
{
  return 0;
}
#endif

#ifdef AUDIAKI_HAVE_JACK
const aud_capture_ops aud_capture_ops_jack = {"jack", NULL, NULL, NULL, NULL,
                                              NULL,   NULL, NULL, NULL, NULL};
const aud_monitor_ops aud_monitor_ops_jack = {"jack", NULL, NULL, NULL,
                                              NULL,   NULL, NULL, NULL};
int aud_jack_server_responds(void)
{
  return 0;
}
#endif

#ifdef AUDIAKI_HAVE_COREAUDIO
const aud_capture_ops aud_capture_ops_coreaudio = {"coreaudio", NULL, NULL, NULL, NULL,
                                                   NULL,        NULL, NULL, NULL, NULL};
const aud_monitor_ops aud_monitor_ops_coreaudio = {"coreaudio", NULL, NULL, NULL,
                                                   NULL,        NULL, NULL, NULL};
#endif

/* -- running one invocation ------------------------------------------------ */

/*
 * The diagnostics, caught rather than let out.
 *
 * Two reasons. A suite that printed thirty refusals between its own lines would
 * be unreadable, and - the point - what a refusal actually says is worth
 * asserting. "--bits only applies to --render" and "unexpected argument" are
 * both exit code 2, and only one of them is the right answer.
 */
static char g_said[4096];
static int g_log_fd = -1;

/*
 * Read back through the descriptor rather than through a FILE*: stderr was
 * pointed at this file with dup2(), so the two share one offset and a stdio
 * stream layered over it would be reading from wherever it last thought it was.
 */
static void say_start(void)
{
  g_said[0] = '\0';
  fflush(stderr);
  if (ftruncate(g_log_fd, 0) != 0 || lseek(g_log_fd, 0, SEEK_SET) < 0)
  {
    return; /* the assertions below are what will say so */
  }
}

static void say_end(void)
{
  ssize_t n;

  fflush(stderr);
  if (lseek(g_log_fd, 0, SEEK_SET) < 0)
  {
    return;
  }
  n = read(g_log_fd, g_said, sizeof(g_said) - 1u);
  g_said[n > 0 ? n : 0] = '\0';
}

/*
 * Parse one invocation, written as it would be typed. `line` is split on
 * spaces, so no test here needs an argument with one in it - and the one place
 * that would matter, a path with a space, is not what any of these are about.
 */
#define MAX_ARGS 24

static int parse(aud_options *opts, const char *line)
{
  static char buf[512];
  static char *argv[MAX_ARGS + 1];
  int argc = 0;
  char *at;
  int rc;

  snprintf(buf, sizeof(buf), "%s", line);

  argv[argc++] = (char *)"audiaki";
  for (at = buf; *at != '\0' && argc < MAX_ARGS;)
  {
    while (*at == ' ')
    {
      *at++ = '\0';
    }
    if (*at == '\0')
    {
      break;
    }
    argv[argc++] = at;
    while (*at != '\0' && *at != ' ')
    {
      at++;
    }
  }
  argv[argc] = NULL;

  say_start();
  rc = cli_parse(argc, argv, opts);
  say_end();
  return rc;
}

/* Whether the last invocation was refused, saying `why`. */
static int refused(int rc, const char *why)
{
  return rc == CLI_EXIT_USAGE && strstr(g_said, why) != NULL;
}

/* -- what is accepted ------------------------------------------------------ */

TEST(a_bare_filename_is_a_recording_at_the_defaults)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_RECORD);
  CHECK_EQ_STR(o.output_path, "take.wav");
  CHECK_EQ_INT(o.rate, AUD_DEFAULT_RATE);
  CHECK_EQ_INT(o.channels, AUD_DEFAULT_CHANNELS);
  CHECK_EQ_INT(o.period_frames, AUD_DEFAULT_PERIOD_FRAMES);
  CHECK_EQ_INT(o.periods, AUD_DEFAULT_PERIODS);
  CHECK_EQ_INT(o.format, AUD_FORMAT_UNKNOWN);
  CHECK_EQ_STR(o.device, AUD_DEFAULT_DEVICE);
  CHECK_EQ_INT(o.device_explicit, 0);
  CHECK_EQ_INT(o.show_meter, 1);
  CHECK_EQ_INT(o.metadata, 1);
  CHECK_EQ_DBL(o.duration, 0.0, 0.0);
  CHECK_EQ_DBL(o.input_gain, 1.0, 0.0);
}

TEST(the_capture_options_reach_the_request)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "-D hw:1,0 -r 48000 -c 4 -f S24 -t 30 -p 256 -n 8 take.wav"), 0);
  CHECK_EQ_STR(o.device, "hw:1,0");
  CHECK_EQ_INT(o.device_explicit, 1);
  CHECK_EQ_INT(o.rate, 48000);
  CHECK_EQ_INT(o.channels, 4);
  CHECK_EQ_INT(o.format, AUD_FORMAT_S24_3LE);
  CHECK_EQ_DBL(o.duration, 30.0, 1e-9);
  CHECK_EQ_INT(o.period_frames, 256);
  CHECK_EQ_INT(o.periods, 8);

  /* and the long spellings mean the same thing */
  CHECK_EQ_INT(parse(&o, "--device hw:1,0 --rate 48000 --channels 4 take.wav"), 0);
  CHECK_EQ_STR(o.device, "hw:1,0");
  CHECK_EQ_INT(o.rate, 48000);
  CHECK_EQ_INT(o.channels, 4);
}

TEST(each_command_is_reached_by_its_own_option)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--list"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_LIST);

  CHECK_EQ_INT(parse(&o, "--probe"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_PROBE);

  CHECK_EQ_INT(parse(&o, "--info take.wav"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_INFO);
  CHECK_EQ_STR(o.input_path, "take.wav");

  CHECK_EQ_INT(parse(&o, "--play take.wav"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_PLAY);

  CHECK_EQ_INT(parse(&o, "--visualize take.wav"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_VISUALIZE);

  CHECK_EQ_INT(parse(&o, "--render session.aki"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_RENDER);

  CHECK_EQ_INT(parse(&o, "--tune"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_TUNE);

  CHECK_EQ_INT(parse(&o, "--calibrate"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_CALIBRATE);

  /* the two that answer and stop, before anything else is looked at */
  CHECK_EQ_INT(parse(&o, "--help"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_HELP);

  CHECK_EQ_INT(parse(&o, "--version"), 0);
  CHECK_EQ_INT(o.command, AUD_CMD_VERSION);
}

TEST(a_glob_after_info_or_play_becomes_the_rest_of_the_list)
{
  aud_options o;

  /*
   * 'audiaki --info session-*.wav' is how anyone would ask: the shell hands
   * over a list, the first goes to --info and the rest have to be picked up
   * from here or the other takes are silently not measured.
   */
  CHECK_EQ_INT(parse(&o, "--info one.wav two.wav three.wav"), 0);
  CHECK_EQ_STR(o.input_path, "one.wav");
  CHECK_EQ_INT(o.extra_input_count, 2);
  CHECK_EQ_STR(o.extra_inputs[0], "two.wav");
  CHECK_EQ_STR(o.extra_inputs[1], "three.wav");

  CHECK_EQ_INT(parse(&o, "--play a.wav b.wav"), 0);
  CHECK_EQ_INT(o.extra_input_count, 1);

  /* and one file alone leaves nothing over rather than a stale list */
  CHECK_EQ_INT(parse(&o, "--info one.wav"), 0);
  CHECK_EQ_INT(o.extra_input_count, 0);
}

TEST(the_metronome_is_turned_on_by_its_tempo_and_shaped_after)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--click 120 --click-beats 3 --click-subdiv 2 "
                         "--click-gain 0.5 take.wav"),
               0);
  CHECK_EQ_DBL(o.click_bpm, 120.0, 1e-9);
  CHECK_EQ_INT(o.click_beats, 3);
  CHECK_EQ_INT(o.click_subdiv, 2);
  CHECK_EQ_DBL(o.click_gain, 0.5, 1e-9);

  /* --metronome is the same option under the name half the world uses */
  CHECK_EQ_INT(parse(&o, "--metronome 90 take.wav"), 0);
  CHECK_EQ_DBL(o.click_bpm, 90.0, 1e-9);
}

TEST(a_single_channel_can_be_picked_out_or_mixed_down)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--channel 2 take.wav"), 0);
  CHECK_EQ_INT(o.channel, 2);

  CHECK_EQ_INT(parse(&o, "--channel mix take.wav"), 0);
  CHECK_EQ_INT(o.channel, AUD_CHANNEL_MIX);

  /* counting from one, so zero is a mistake rather than "all of them" */
  CHECK(refused(parse(&o, "--channel 0 take.wav"), "--channel"));
}

TEST(the_verbosity_flags_move_the_log_level_either_way)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_INT(o.log_level, AUD_LOG_NORMAL);

  CHECK_EQ_INT(parse(&o, "-q take.wav"), 0);
  CHECK_EQ_INT(o.log_level, AUD_LOG_QUIET);

  CHECK_EQ_INT(parse(&o, "-v take.wav"), 0);
  CHECK_EQ_INT(o.log_level, AUD_LOG_VERBOSE);
}

/* -- values that are out of range ------------------------------------------ */

TEST(a_value_that_is_not_a_number_is_refused_by_name)
{
  aud_options o;

  /*
   * The one case CI has always covered, and the shape of every other: the
   * option is named in the message, because "invalid value" on its own leaves
   * somebody reading a whole command line looking for it.
   */
  CHECK(refused(parse(&o, "--rate not-a-number take.wav"), "--rate"));
  CHECK(refused(parse(&o, "--channels lots take.wav"), "--channels"));
  CHECK(refused(parse(&o, "--period soon take.wav"), "--period"));
  CHECK(refused(parse(&o, "--duration ages take.wav"), "--duration"));
  CHECK(refused(parse(&o, "--format S99 take.wav"), "--format"));
}

TEST(the_bounds_are_the_ones_the_help_text_quotes)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--rate 4000 take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--rate 768000 take.wav"), 0);
  CHECK(refused(parse(&o, "--rate 3999 take.wav"), "--rate"));
  CHECK(refused(parse(&o, "--rate 768001 take.wav"), "--rate"));

  CHECK_EQ_INT(parse(&o, "--channels 1 take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--channels 64 take.wav"), 0);
  CHECK(refused(parse(&o, "--channels 0 take.wav"), "--channels"));
  CHECK(refused(parse(&o, "--channels 65 take.wav"), "--channels"));

  /*
   * -t 1e308 does not record for a very long time: it reaches record.c as
   * duration * rate cast to a frame count, and a double too large for that
   * conversion is undefined behaviour rather than a long take.
   */
  CHECK(refused(parse(&o, "--duration 1e308 take.wav"), "--duration"));
  CHECK(refused(parse(&o, "--duration -1 take.wav"), "--duration"));
}

TEST(an_odd_video_size_is_caught_here_rather_than_inside_ffmpeg)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--visualize in.wav --size 1280x720"), 0);
  CHECK_EQ_INT(o.viz_width, 1280);
  CHECK_EQ_INT(o.viz_height, 720);

  /*
   * yuv420p subsamples by two both ways, so an odd dimension fails deep inside
   * ffmpeg with a message about pixel formats. Saying it here says what is
   * actually wrong.
   */
  CHECK(refused(parse(&o, "--visualize in.wav --size 1281x720"), "even"));
  CHECK(refused(parse(&o, "--visualize in.wav --size 1280x721"), "even"));
}

TEST(an_inverted_tuner_range_is_refused_before_the_tuner_sees_it)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--tune --tune-min 80 --tune-max 400"), 0);
  CHECK_EQ_DBL(o.tune_min_hz, 80.0, 1e-9);
  CHECK_EQ_DBL(o.tune_max_hz, 400.0, 1e-9);

  /* less than an octave apart is a range the analysis cannot work in */
  CHECK(refused(parse(&o, "--tune --tune-min 200 --tune-max 300"), "octave"));
  CHECK(refused(parse(&o, "--tune --tune-min 400 --tune-max 80"), "octave"));
}

TEST(an_unknown_option_and_a_missing_argument_are_both_refused)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--nonsense take.wav"), CLI_EXIT_USAGE);
  CHECK_EQ_INT(parse(&o, "--rate"), CLI_EXIT_USAGE);
  CHECK_EQ_INT(parse(&o, "-r"), CLI_EXIT_USAGE);
}

/* -- an option on a command it means nothing to ---------------------------- */

/*
 * The rules with the most reason to exist and the least to announce themselves.
 * Each of these was, or would have been, silently ignored - the invocation
 * would run and do something subtly other than what was typed.
 */
TEST(an_option_that_only_records_is_refused_on_a_command_that_does_not)
{
  aud_options o;

  CHECK(refused(parse(&o, "--info in.wav --take song"), "--take"));
  CHECK(refused(parse(&o, "--info in.wav --dir ~/takes"), "--dir"));
  CHECK(refused(parse(&o, "--info in.wav --preroll 5"), "--preroll"));
  CHECK(refused(parse(&o, "--info in.wav --note hello"), "--note"));
  CHECK(refused(parse(&o, "--info in.wav --no-metadata"), "--no-metadata"));
  CHECK(refused(parse(&o, "--info in.wav --click 120"), "--click"));
  CHECK(refused(parse(&o, "--info in.wav --monitor"), "--monitor"));

  /* and every one of them is fine on the command that does record */
  CHECK_EQ_INT(parse(&o, "--take song"), 0);
  CHECK_EQ_INT(parse(&o, "--dir takes take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--preroll 5 take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--note hello take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--no-metadata take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--click 120 take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "-M take.wav"), 0);
}

TEST(playing_something_already_is_not_monitoring_it)
{
  aud_options o;

  /*
   * Its own message rather than the general one: --play is already playing
   * through an output, and the answer to "which one" is -D rather than a flag
   * that means something else entirely.
   */
  CHECK(refused(parse(&o, "--play a.wav --monitor"), "name it with -D"));
}

TEST(json_is_refused_on_a_command_with_nothing_to_serialise)
{
  aud_options o;

  /*
   * Ignoring it would let a script believe it was getting parseable output
   * right up until it tried to parse a peak meter.
   */
  CHECK_EQ_INT(parse(&o, "--list --json"), 0);
  CHECK_EQ_INT(o.json, 1);
  CHECK_EQ_INT(parse(&o, "--probe --json"), 0);
  CHECK_EQ_INT(parse(&o, "--info in.wav --json"), 0);

  CHECK(refused(parse(&o, "--json take.wav"), "--json only applies"));
  CHECK(refused(parse(&o, "--play a.wav --json"), "--json only applies"));
}

TEST(the_render_only_options_are_refused_everywhere_else)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--render s.aki --bits 24"), 0);
  CHECK_EQ_INT(o.export_bits, 24);
  CHECK_EQ_INT(parse(&o, "--render s.aki --stems"), 0);
  CHECK_EQ_INT(o.export_stems, 1);

  /*
   * Checked with the cross-command rules rather than among the per-command
   * ones, because those return before anything after them is reached - which
   * is how --bits with --info was accepted in silence for a while.
   */
  CHECK(refused(parse(&o, "--info in.wav --bits 24"), "--bits only applies"));
  CHECK(refused(parse(&o, "--bits 24 take.wav"), "--bits only applies"));
  CHECK(refused(parse(&o, "--info in.wav --stems"), "--stems only applies"));
  CHECK(refused(parse(&o, "--stems take.wav"), "--stems only applies"));
}

TEST(a_playlist_option_needs_a_playlist)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--play a.wav --shuffle"), 0);
  CHECK_EQ_INT(o.shuffle, 1);
  CHECK_EQ_INT(parse(&o, "--play a.wav --repeat"), 0);
  CHECK_EQ_INT(o.repeat, AUD_REPEAT_ALL);
  CHECK_EQ_INT(parse(&o, "--play a.wav --repeat-one"), 0);
  CHECK_EQ_INT(o.repeat, AUD_REPEAT_ONE);

  CHECK(refused(parse(&o, "--shuffle take.wav"), "only --play has one"));
  CHECK(refused(parse(&o, "--repeat take.wav"), "only --play has one"));
}

TEST(the_click_shape_options_need_a_click_to_shape)
{
  aud_options o;

  CHECK(refused(parse(&o, "--click-beats 3 take.wav"), "--click sets its tempo"));
  CHECK(refused(parse(&o, "--click-subdiv 2 take.wav"), "--click sets its tempo"));
  CHECK(refused(parse(&o, "--click-gain 0.5 take.wav"), "--click sets its tempo"));
  CHECK_EQ_INT(parse(&o, "--click 120 --click-beats 3 take.wav"), 0);
}

TEST(calibrate_measures_the_number_latency_would_have_told_it)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--calibrate"), 0);

  /*
   * --latency only moves the click off the beat, so like the other three that
   * shape a metronome it needs one to shape - and the message has to name it,
   * or somebody who typed --latency is told about --click-gain.
   */
  CHECK_EQ_INT(parse(&o, "--click 120 --latency 12 take.wav"), 0);
  CHECK_EQ_DBL(o.latency_ms, 12.0, 1e-9);
  CHECK(refused(parse(&o, "--latency 12 take.wav"), "--latency"));

  /* with --calibrate the two are a contradiction rather than a spare option */
  CHECK(refused(parse(&o, "--calibrate --latency 12"), "is what it measures"));
}

TEST(the_prompt_pair_applies_where_there_is_a_question_to_ask)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--prompt take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--no-prompt take.wav"), 0);
  CHECK_EQ_INT(parse(&o, "--prompt --calibrate"), 0);

  CHECK(refused(parse(&o, "--info in.wav --prompt"), "recording or calibrating"));
}

/* -- files named, and files named twice ------------------------------------ */

TEST(a_command_that_writes_nothing_refuses_a_file_to_write)
{
  aud_options o;

  CHECK(refused(parse(&o, "--tune take.wav"), "--tune records nothing"));
  CHECK(refused(parse(&o, "--tune -o take.wav"), "--tune records nothing"));
  CHECK(refused(parse(&o, "--calibrate take.wav"), "--calibrate records nothing"));
  CHECK(refused(parse(&o, "--calibrate -o take.wav"), "--calibrate records nothing"));
  CHECK(refused(parse(&o, "--play a.wav -o out.wav"), "--play writes nothing"));
}

TEST(naming_the_output_twice_is_a_mistake_rather_than_a_preference)
{
  aud_options o;

  /* -o is accepted for a recording too, so both spellings work on their own */
  CHECK_EQ_INT(parse(&o, "-o take.wav"), 0);
  CHECK_EQ_STR(o.output_path, "take.wav");
  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_STR(o.output_path, "take.wav");

  CHECK(refused(parse(&o, "-o one.wav two.wav"), "output file given twice"));
  CHECK(refused(parse(&o, "one.wav two.wav"), "only one output file"));
}

TEST(recording_needs_somewhere_to_record_to)
{
  aud_options o;

  CHECK(refused(parse(&o, ""), "no output file given"));

  /* --take names it instead, which is the other way to answer the question */
  CHECK_EQ_INT(parse(&o, "--take song"), 0);
  CHECK_EQ_STR(o.take_prefix, "song");

  /* ...and answering it both ways at once is a conflict */
  CHECK(refused(parse(&o, "--take song take.wav"), "both name the take"));
}

TEST(a_leftover_argument_is_refused_by_the_command_it_was_left_over_from)
{
  aud_options o;

  CHECK(refused(parse(&o, "--visualize in.wav extra.mp4"), "the output from -o"));
  CHECK(refused(parse(&o, "--render s.aki extra.wav"), "the mix from -o"));
  CHECK(refused(parse(&o, "--list extra"), "unexpected argument"));
}

/* -- the backend ----------------------------------------------------------- */

TEST(a_backend_is_named_whether_or_not_this_build_has_it)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "--backend auto take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_AUTO);

  /*
   * Every name parses here, including one this build was compiled without: the
   * refusal for that belongs to selection, where it can say "this build has no
   * jack backend" rather than "invalid value", which reads as a typo.
   */
  CHECK_EQ_INT(parse(&o, "--backend alsa take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_ALSA);
  CHECK_EQ_INT(parse(&o, "--backend pipewire take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_PIPEWIRE);
  CHECK_EQ_INT(parse(&o, "--backend pw take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_PIPEWIRE);
  CHECK_EQ_INT(parse(&o, "--backend jack take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_JACK);
  CHECK_EQ_INT(parse(&o, "--backend coreaudio take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_COREAUDIO);
  CHECK_EQ_INT(parse(&o, "--backend ca take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_COREAUDIO);

  CHECK(refused(parse(&o, "--backend nonsense take.wav"), "--backend"));
}

TEST(the_environment_is_a_default_and_the_command_line_still_wins)
{
  aud_options o;

  setenv("AUDIAKI_DEVICE", "hw:9,9", 1);
  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_STR(o.device, "hw:9,9");
  /* it was not typed, and --play has to be able to tell those apart */
  CHECK_EQ_INT(o.device_explicit, 0);

  CHECK_EQ_INT(parse(&o, "-D hw:1,0 take.wav"), 0);
  CHECK_EQ_STR(o.device, "hw:1,0");
  CHECK_EQ_INT(o.device_explicit, 1);
  unsetenv("AUDIAKI_DEVICE");

  setenv("AUDIAKI_BACKEND", "alsa", 1);
  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_ALSA);

  /*
   * A typo in an exported variable is left at auto rather than rejected. The
   * alternative is every invocation on that machine failing, including the
   * --help that would explain the spelling.
   */
  setenv("AUDIAKI_BACKEND", "nonsense", 1);
  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_INT(o.backend, AUD_BACKEND_AUTO);
  unsetenv("AUDIAKI_BACKEND");
}

/* -- one parse does not leak into the next --------------------------------- */

TEST(a_rejected_invocation_leaves_nothing_behind_for_the_next_one)
{
  aud_options o;

  /*
   * The parser stops part way through a rejected command line, and getopt keeps
   * where it got to in a global. Without putting that back, the invocation
   * after a refusal reads the wrong arguments - which is invisible in a program
   * that parses once and is exactly what these tests do a hundred times.
   */
  CHECK(refused(parse(&o, "--rate nope take.wav"), "--rate"));
  CHECK_EQ_INT(parse(&o, "-r 48000 take.wav"), 0);
  CHECK_EQ_INT(o.rate, 48000);
  CHECK_EQ_STR(o.output_path, "take.wav");

  CHECK_EQ_INT(parse(&o, "--nonsense"), CLI_EXIT_USAGE);
  CHECK_EQ_INT(parse(&o, "--info a.wav b.wav"), 0);
  CHECK_EQ_INT(o.extra_input_count, 1);
  CHECK_EQ_STR(o.extra_inputs[0], "b.wav");
}

TEST(every_parse_starts_from_the_defaults_rather_than_the_last_one)
{
  aud_options o;

  CHECK_EQ_INT(parse(&o, "-r 96000 -c 8 --shuffle --repeat --play a.wav b.wav"), 0);
  CHECK_EQ_INT(o.rate, 96000);

  CHECK_EQ_INT(parse(&o, "take.wav"), 0);
  CHECK_EQ_INT(o.rate, AUD_DEFAULT_RATE);
  CHECK_EQ_INT(o.channels, AUD_DEFAULT_CHANNELS);
  CHECK_EQ_INT(o.monitor, 0);
  CHECK_EQ_INT(o.shuffle, 0);
  CHECK_EQ_INT(o.command, AUD_CMD_RECORD);
  CHECK(o.input_path == NULL);
  CHECK_EQ_INT(o.extra_input_count, 0);
}

int main(void)
{
  /*
   * No config file and no environment, so what these read is the parser's own
   * defaults rather than whatever is on the machine running them.
   */
  setenv("AUDIAKI_CONFIG", "/nonexistent/audiaki-test.conf", 1);
  unsetenv("AUDIAKI_DEVICE");
  unsetenv("AUDIAKI_BACKEND");

  {
    FILE *log = tmpfile();

    if (log == NULL || dup2(fileno(log), STDERR_FILENO) < 0)
    {
      printf("  FAIL cannot redirect the diagnostics\n");
      return 1;
    }
    g_log_fd = fileno(log);
  }

  RUN(a_bare_filename_is_a_recording_at_the_defaults);
  RUN(the_capture_options_reach_the_request);
  RUN(each_command_is_reached_by_its_own_option);
  RUN(a_glob_after_info_or_play_becomes_the_rest_of_the_list);
  RUN(the_metronome_is_turned_on_by_its_tempo_and_shaped_after);
  RUN(a_single_channel_can_be_picked_out_or_mixed_down);
  RUN(the_verbosity_flags_move_the_log_level_either_way);
  RUN(a_value_that_is_not_a_number_is_refused_by_name);
  RUN(the_bounds_are_the_ones_the_help_text_quotes);
  RUN(an_odd_video_size_is_caught_here_rather_than_inside_ffmpeg);
  RUN(an_inverted_tuner_range_is_refused_before_the_tuner_sees_it);
  RUN(an_unknown_option_and_a_missing_argument_are_both_refused);
  RUN(an_option_that_only_records_is_refused_on_a_command_that_does_not);
  RUN(playing_something_already_is_not_monitoring_it);
  RUN(json_is_refused_on_a_command_with_nothing_to_serialise);
  RUN(the_render_only_options_are_refused_everywhere_else);
  RUN(a_playlist_option_needs_a_playlist);
  RUN(the_click_shape_options_need_a_click_to_shape);
  RUN(calibrate_measures_the_number_latency_would_have_told_it);
  RUN(the_prompt_pair_applies_where_there_is_a_question_to_ask);
  RUN(a_command_that_writes_nothing_refuses_a_file_to_write);
  RUN(naming_the_output_twice_is_a_mistake_rather_than_a_preference);
  RUN(recording_needs_somewhere_to_record_to);
  RUN(a_leftover_argument_is_refused_by_the_command_it_was_left_over_from);
  RUN(a_backend_is_named_whether_or_not_this_build_has_it);
  RUN(the_environment_is_a_default_and_the_command_line_still_wins);
  RUN(a_rejected_invocation_leaves_nothing_behind_for_the_next_one);
  RUN(every_parse_starts_from_the_defaults_rather_than_the_last_one);
  return TEST_RESULT();
}
