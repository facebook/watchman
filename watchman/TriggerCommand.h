/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thread>

#include "watchman/ChildProcess.h"
#include "watchman/PubSub.h"

class watchman_event;
struct watchman_root;
struct w_query;

namespace watchman {

enum trigger_input_style { input_dev_null, input_json, input_name_list };

struct TriggerCommand {
  w_string triggername;
  std::shared_ptr<w_query> query;
  json_ref definition;
  json_ref command;
  watchman::ChildProcess::Environment env;

  bool append_files;
  enum trigger_input_style stdin_style;
  uint32_t max_files_stdin;

  int stdout_flags;
  int stderr_flags;
  std::string stdout_name;
  std::string stderr_name;

  /* While we are running, this holds the pid
   * of the running process */
  std::unique_ptr<watchman::ChildProcess> current_proc;

  TriggerCommand(
      const std::shared_ptr<watchman_root>& root,
      const json_ref& trig);
  ~TriggerCommand();

  void stop();
  void start(const std::shared_ptr<watchman_root>& root);

 private:
  TriggerCommand(const TriggerCommand&) = delete;
  TriggerCommand(TriggerCommand&&) = delete;

  TriggerCommand& operator=(const TriggerCommand&) = delete;
  TriggerCommand& operator=(TriggerCommand&&) = delete;

  std::thread triggerThread_;
  std::shared_ptr<watchman::Publisher::Subscriber> subscriber_;
  std::unique_ptr<watchman_event> ping_;
  bool stopTrigger_{false};

  void run(const std::shared_ptr<watchman_root>& root);
  bool maybeSpawn(const std::shared_ptr<watchman_root>& root);
  bool waitNoIntr();
};

} // namespace watchman
