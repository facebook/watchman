/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include "make_unique.h"
#include "watchman.h"

using watchman::ChildProcess;
using watchman::FileDescriptor;
using Options = watchman::ChildProcess::Options;
using Environment = watchman::ChildProcess::Environment;

static std::unique_ptr<watchman_stream> prepare_stdin(
    struct watchman_trigger_command* cmd,
    w_query_res* res) {
  char stdin_file_name[WATCHMAN_NAME_MAX];

  if (cmd->stdin_style == trigger_input_style::input_dev_null) {
    return w_stm_open("/dev/null", O_RDONLY|O_CLOEXEC);
  }

  // Adjust result to fit within the specified limit
  if (cmd->max_files_stdin > 0) {
    auto& fileList = res->resultsArray.array();
    auto n_files = std::min(size_t(cmd->max_files_stdin), fileList.size());
    fileList.resize(std::min(fileList.size(), n_files));
  }

  /* prepare the input stream for the child process */
  snprintf(
      stdin_file_name,
      sizeof(stdin_file_name),
      "%s/wmanXXXXXX",
      watchman_tmp_dir);
  auto stdin_file = w_mkstemp(stdin_file_name);
  if (!stdin_file) {
    w_log(W_LOG_ERR, "unable to create a temporary file: %s %s\n",
        stdin_file_name, strerror(errno));
    return NULL;
  }

  /* unlink the file, we don't need it in the filesystem;
   * we'll pass the fd on to the child as stdin */
  unlink(stdin_file_name); // FIXME: windows path translation

  switch (cmd->stdin_style) {
    case input_json:
      {
        w_jbuffer_t buffer;

        w_log(W_LOG_ERR, "input_json: sending json object to stm\n");
        if (!buffer.jsonEncodeToStream(
                res->resultsArray, stdin_file.get(), 0)) {
          w_log(W_LOG_ERR,
              "input_json: failed to write json data to stream: %s\n",
              strerror(errno));
          return NULL;
        }
        break;
      }
    case input_name_list:
      for (auto& name : res->resultsArray.array()) {
        auto& nameStr = json_to_w_string(name);
        if (stdin_file->write(nameStr.data(), nameStr.size()) !=
                (int)nameStr.size() ||
            stdin_file->write("\n", 1) != 1) {
          w_log(
              W_LOG_ERR,
              "write failure while producing trigger stdin: %s\n",
              strerror(errno));
          return nullptr;
        }
      }
      break;
    case input_dev_null:
      // already handled above
      break;
  }

  stdin_file->rewind();
  return stdin_file;
}

static void spawn_command(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_trigger_command* cmd,
    w_query_res* res,
    struct ClockSpec* since_spec) {
  long arg_max;
  size_t argspace_remaining;
  bool file_overflow = false;

#ifdef _WIN32
  arg_max = 32*1024;
#else
  arg_max = sysconf(_SC_ARG_MAX);
#endif

  if (arg_max <= 0) {
    argspace_remaining = UINT_MAX;
  } else {
    argspace_remaining = (uint32_t)arg_max;
  }

  // Allow some misc working overhead
  argspace_remaining -= 32;

  // Record an overflow before we call prepare_stdin(), which mutates
  // and resizes the results to fit the specified limit.
  if (cmd->max_files_stdin > 0 &&
      res->resultsArray.array().size() > cmd->max_files_stdin) {
    file_overflow = true;
  }

  auto stdin_file = prepare_stdin(cmd, res);
  if (!stdin_file) {
    w_log(
        W_LOG_ERR,
        "trigger %s:%s %s\n",
        root->root_path.c_str(),
        cmd->triggername.c_str(),
        strerror(errno));
    return;
  }

  // Assumption: that only one thread will be executing on a given
  // cmd instance so that mutation of cmd->env is safe.
  // This is guaranteed in the current architecture.

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock) {
    cmd->env.set("WATCHMAN_SINCE", since_spec->clock.position.toClockString());
  } else {
    cmd->env.unset("WATCHMAN_SINCE");
  }

  cmd->env.set(
      "WATCHMAN_CLOCK", res->clockAtStartOfQuery.position().toClockString());

  if (cmd->query->relative_root) {
    cmd->env.set("WATCHMAN_RELATIVE_ROOT", cmd->query->relative_root);
  } else {
    cmd->env.unset("WATCHMAN_RELATIVE_ROOT");
  }

  // Compute args
  auto args = json_deep_copy(cmd->command);

  if (cmd->append_files) {
    // Measure how much space the base args take up
    for (size_t i = 0; i < json_array_size(args); i++) {
      const char *ele = json_string_value(json_array_get(args, i));

      argspace_remaining -= strlen(ele) + 1 + sizeof(char*);
    }

    // Dry run with env to compute space
    size_t env_size;
    cmd->env.asEnviron(&env_size);
    argspace_remaining -= env_size;

    for (const auto& item : res->dedupedFileNames) {
      // also: NUL terminator and entry in argv
      uint32_t size = item.size() + 1 + sizeof(char*);

      if (argspace_remaining < size) {
        file_overflow = true;
        break;
      }
      argspace_remaining -= size;

      json_array_append_new(args, w_string_to_json(item));
    }
  }

  cmd->env.set("WATCHMAN_FILES_OVERFLOW", file_overflow);

  Options opts;
  opts.environment() = cmd->env;
#ifndef _WIN32
  sigset_t mask;
  sigemptyset(&mask);
  opts.setSigMask(mask);
#endif
  opts.setFlags(POSIX_SPAWN_SETPGROUP);

  opts.dup2(stdin_file->getFileDescriptor(), STDIN_FILENO);

  if (cmd->stdout_name) {
    opts.open(STDOUT_FILENO, cmd->stdout_name, cmd->stdout_flags, 0666);
  } else {
    opts.dup2(FileDescriptor::stdOut(), STDOUT_FILENO);
  }

  if (cmd->stderr_name) {
    opts.open(STDOUT_FILENO, cmd->stderr_name, cmd->stderr_flags, 0666);
  } else {
    opts.dup2(FileDescriptor::stdErr(), STDERR_FILENO);
  }

  // Figure out the appropriate cwd
  w_string working_dir(cmd->query->relative_root);
  if (!working_dir) {
    working_dir = root->root_path;
  }

  auto cwd = cmd->definition.get_default("chdir");
  if (cwd) {
    auto target = json_to_w_string(cwd);
    if (w_is_path_absolute_cstr_len(target.data(), target.size())) {
      working_dir = target;
    } else {
      working_dir = w_string::pathCat({working_dir, target});
    }
  }

  watchman::log(watchman::DBG, "using ", working_dir, " for working dir\n");
  opts.chdir(working_dir.c_str());

  try {
    if (cmd->current_proc) {
      cmd->current_proc->kill();
      cmd->current_proc->wait();
    }
    cmd->current_proc =
        watchman::make_unique<ChildProcess>(args, std::move(opts));
  } catch (const std::exception& exc) {
    watchman::log(
        watchman::ERR,
        "trigger ",
        root->root_path,
        ":",
        cmd->triggername,
        " failed: ",
        exc.what(),
        "\n");
  }

  // We have integration tests that check for this string
  watchman::log(
      cmd->current_proc ? watchman::DBG : watchman::ERR,
      "posix_spawnp: ",
      cmd->triggername,
      "\n");
}

bool watchman_trigger_command::maybeSpawn(
    const std::shared_ptr<w_root_t>& root) {
  bool didRun = false;

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer triggers until
  // things settle down
  if (root->view()->isVCSOperationInProgress()) {
    w_log(W_LOG_DBG, "deferring triggers until VCS operations complete\n");
    return false;
  }

  auto since_spec = query->since_spec.get();

  if (since_spec && since_spec->tag == w_cs_clock) {
    w_log(
        W_LOG_DBG,
        "running trigger \"%s\" rules! since %" PRIu32 "\n",
        triggername.c_str(),
        since_spec->clock.position.ticks);
  } else {
    w_log(W_LOG_DBG, "running trigger \"%s\" rules!\n", triggername.c_str());
  }

  // Triggers never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  query->sync_timeout = std::chrono::milliseconds(0);
  watchman::log(watchman::DBG, "assessing trigger ", triggername, "\n");
  try {
    auto res = w_query_execute(query.get(), root, time_generator);

    watchman::log(
        watchman::DBG,
        "trigger \"",
        triggername,
        "\" generated ",
        res.resultsArray.array().size(),
        " results\n");

    // create a new spec that will be used the next time
    auto saved_spec = std::move(query->since_spec);
    query->since_spec =
        watchman::make_unique<ClockSpec>(res.clockAtStartOfQuery);

    watchman::log(
        watchman::DBG,
        "updating trigger \"",
        triggername,
        "\" use ",
        res.clockAtStartOfQuery.position().ticks,
        " ticks next time\n");

    if (!res.resultsArray.array().empty()) {
      didRun = true;
      spawn_command(root, this, &res, saved_spec.get());
    }
    return didRun;
  } catch (const QueryExecError& e) {
    watchman::log(
        watchman::ERR,
        "error running trigger \"",
        triggername,
        "\" query: ",
        e.what(),
        "\n");
    return false;
  }
}

/* vim:ts=2:sw=2:et:
 */
