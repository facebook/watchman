/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

enum argtype {
  OPT_NONE,
  REQ_STRING,
  REQ_INT,
};

struct watchman_getopt {
  /* name of long option: --optname */
  const char *optname;
  /* if non-zero, short option character */
  int shortopt;
  /* help text shown in the usage information */
  const char *helptext;
  /* whether we accept an argument */
  enum argtype argtype;
  /* if an argument was provided, *val will be set to
   * point to the option value.
   * Because we only update the option if one was provided
   * by the user, you can safely pre-initialize the val
   * pointer to your choice of default.
   * */
  void *val;

  /* if argtype != OPT_NONE, this is the label used to
   * refer to the argument in the help text.  If left
   * blank, we'll use the string "ARG" as a generic
   * alternative */
  const char *arglabel;

  // Whether this option should be passed to the child
  // when running under the gimli monitor
  int is_daemon;
#define IS_DAEMON 1
#define NOT_DAEMON 0
};

bool w_getopt(
    struct watchman_getopt* opts,
    int* argcp,
    char*** argvp,
    char*** daemon_argv);
void usage(struct watchman_getopt* opts, FILE* where);
void print_command_list_for_help(FILE* where);
