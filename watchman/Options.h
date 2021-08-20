/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdio.h>
#include <string>

namespace watchman {

struct Flags {
  int show_help = 0;
#ifndef _WIN32
  int inetd_style = 0;
#endif
  int no_site_spawner = 0;
  int show_version = 0;
  std::string named_pipe_path;
  std::string unix_sock_name;
  int enable_tcp = 0;
  std::string tcp_host;
  std::string pid_file;
  int persistent = 0;
  int dont_save_state = 0;
  std::string watchman_state_file;
  int json_input_arg = 0;
  std::string output_encoding;
  std::string server_encoding;
  int foreground = 0;
  int no_pretty = 0;
  int no_spawn = 0;
  int no_local = 0;
  std::string test_state_dir;
};

extern Flags flags;

enum ArgType {
  OPT_NONE,
  REQ_STRING,
  REQ_INT,
};

struct OptDesc {
  /* name of long option: --optname */
  const char* optname;
  /* if non-zero, short option character */
  int shortopt;
  /* help text shown in the usage information */
  const char* helptext;
  /* whether we accept an argument */
  ArgType argtype;
  /* if an argument was provided, *val will be set to
   * point to the option value.
   * Because we only update the option if one was provided
   * by the user, you can safely pre-initialize the val
   * pointer to your choice of default.
   * */
  void* val;

  /* if argtype != OPT_NONE, this is the label used to
   * refer to the argument in the help text.  If left
   * blank, we'll use the string "ARG" as a generic
   * alternative */
  const char* arglabel;

  // Whether this option should be passed to the child
  // when spawning the daemon
  int is_daemon;
#define IS_DAEMON 1
#define NOT_DAEMON 0
};

/**
 * Populates the globals in `flags`.
 */
void parseOptions(int* argcp, char*** argvp, char*** daemon_argv);

// The following are largely for internal use.

bool w_getopt(OptDesc* opts, int* argcp, char*** argvp, char*** daemon_argv);
[[noreturn]] void usage(struct watchman_getopt* opts, FILE* where);
void print_command_list_for_help(FILE* where);

} // namespace watchman
