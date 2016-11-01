/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once

enum trigger_input_style { input_dev_null, input_json, input_name_list };

struct watchman_trigger_command {
  w_string triggername;
  std::shared_ptr<w_query> query;
  json_ref definition;
  json_ref command;
  std::unordered_map<w_string, w_string> envht;

  struct w_query_field_list field_list;
  int append_files;
  enum trigger_input_style stdin_style;
  uint32_t max_files_stdin;

  int stdout_flags;
  int stderr_flags;
  const char *stdout_name;
  const char *stderr_name;

  /* While we are running, this holds the pid
   * of the running process */
  pid_t current_proc;

  watchman_trigger_command();
  watchman_trigger_command(const watchman_trigger_command&) = delete;
};

void w_assess_trigger(
    struct read_locked_watchman_root* lock,
    struct watchman_trigger_command* cmd);
std::unique_ptr<watchman_trigger_command> w_build_trigger_from_def(
    const w_root_t* root,
    const json_ref& trig,
    char** errmsg);
