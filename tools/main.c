/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"

#define PROGRAM_NAME "runshot"
#define COMMAND_INFO "info"
#define COMMAND_PLAY "play"
#define COMMAND_ALL  (COMMAND_INFO " | " COMMAND_PLAY)

struct CommandOptions
{
    int verbose;
    char *file_name;
};

/*
 * Command: info
 */
static char info_cmd_doc[] = "Show information of a snapshot file";
static char info_args_doc[] = "<snapshot file>";

static struct argp_option info_options[] = {
    {"verbose", 'v', 0, 0, "Produce verbose output"}, {0}};

static error_t
parse_info_option(int key, char *arg, struct argp_state *state)
{
    struct CommandOptions *options = state->input;

    switch (key) {
    case 'v':
        options->verbose = 1;
        break;
    case ARGP_KEY_ARG:
        if (state->arg_num > 0) {
            argp_error(state, "too many arguments");
        }
        options->file_name = arg;
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1) {
            argp_error(state, "not enough arguments");
        }
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp info_argp = {info_options, parse_info_option, info_args_doc,
                                info_cmd_doc};

/*
 * Command: play
 */
static char play_cmd_doc[] = "Play back a snapshot file";
static char play_args_doc[] = "<snapshot file>";

static struct argp_option play_options[] = {
    {"verbose", 'v', 0, 0, "Produce verbose output"}, {0}};

static error_t
parse_play_option(int key, char *arg, struct argp_state *state)
{
    struct CommandOptions *options = state->input;

    switch (key) {
    case 'v':
        options->verbose = 1;
        break;
    case ARGP_KEY_ARG:
        if (state->arg_num > 0) {
            argp_error(state, "too many arguments");
        }
        options->file_name = arg;
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1) {
            argp_error(state, "not enough arguments");
        }
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp play_argp = {play_options, parse_play_option, play_args_doc,
                                play_cmd_doc};

int
main(int argc, char **argv)
{
    struct CommandOptions options;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> ...\n", argv[0]);
        fprintf(stderr, "Commands: %s\n", COMMAND_ALL);
        return EXIT_FAILURE;
    }

    options.verbose = 0;
    options.file_name = NULL;

    if (strcmp(argv[1], COMMAND_INFO) == 0) {
        argv[1] = PROGRAM_NAME " " COMMAND_INFO;
        argp_parse(&info_argp, argc - 1, argv + 1, 0, 0, &options);
        cmd_info(options.file_name, options.verbose);
    } else if (strcmp(argv[1], COMMAND_PLAY) == 0) {
        argv[1] = PROGRAM_NAME " " COMMAND_PLAY;
        argp_parse(&play_argp, argc - 1, argv + 1, 0, 0, &options);
        cmd_play(options.file_name, options.verbose);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    return 0;
}
