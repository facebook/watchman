/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static w_stm_t prepare_stdin(
  struct watchman_trigger_command *cmd,
  w_query_res *res)
{
  char stdin_file_name[WATCHMAN_NAME_MAX];
  w_stm_t stdin_file = NULL;

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
  snprintf(stdin_file_name, sizeof(stdin_file_name), "%s%cwmanXXXXXX",
      watchman_tmp_dir, WATCHMAN_DIR_SEP);
  stdin_file = w_mkstemp(stdin_file_name);
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

        if (!w_json_buffer_init(&buffer)) {
          w_log(W_LOG_ERR, "failed to init json buffer\n");
          w_stm_close(stdin_file);
          return NULL;
        }

        w_log(W_LOG_ERR, "input_json: sending json object to stm\n");
        if (!w_json_buffer_write(&buffer, stdin_file, res->resultsArray, 0)) {
          w_log(W_LOG_ERR,
              "input_json: failed to write json data to stream: %s\n",
              strerror(errno));
          w_stm_close(stdin_file);
          return NULL;
        }
        w_json_buffer_free(&buffer);
        break;
      }
    case input_name_list:
      for (auto& name : res->resultsArray.array()) {
        auto& nameStr = json_to_w_string(name);
        if (w_stm_write(stdin_file, nameStr.data(), nameStr.size()) !=
                (int)nameStr.size() ||
            w_stm_write(stdin_file, "\n", 1) != 1) {
          w_log(
              W_LOG_ERR,
              "write failure while producing trigger stdin: %s\n",
              strerror(errno));
          w_stm_close(stdin_file);
          return nullptr;
        }
      }
      break;
    case input_dev_null:
      // already handled above
      break;
  }

  w_stm_rewind(stdin_file);
  return stdin_file;
}

static void spawn_command(w_root_t *root,
  struct watchman_trigger_command *cmd,
  w_query_res *res,
  struct w_clockspec *since_spec)
{
  char **envp = NULL;
  uint32_t i = 0;
  int ret;
  w_stm_t stdin_file = NULL;
  char **argv = NULL;
  uint32_t env_size;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
#ifndef _WIN32
  sigset_t mask;
#endif
  long arg_max;
  size_t argspace_remaining;
  bool file_overflow = false;
  int result_log_level;
  char clockbuf[128];
  w_string_t *working_dir = NULL;

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

  stdin_file = prepare_stdin(cmd, res);
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
  // cmd instance so that mutation of cmd->envht is safe.
  // This is guaranteed in the current architecture.

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock &&
      clock_id_string(since_spec->clock.root_number, since_spec->clock.ticks,
                      clockbuf, sizeof(clockbuf))) {
    w_envp_set_cstring(cmd->envht, "WATCHMAN_SINCE", clockbuf);
  } else {
    w_envp_unset(cmd->envht, "WATCHMAN_SINCE");
  }

  if (clock_id_string(res->root_number, res->ticks,
        clockbuf, sizeof(clockbuf))) {
    w_envp_set_cstring(cmd->envht, "WATCHMAN_CLOCK", clockbuf);
  } else {
    w_envp_unset(cmd->envht, "WATCHMAN_CLOCK");
  }

  if (cmd->query->relative_root) {
    w_envp_set(cmd->envht, "WATCHMAN_RELATIVE_ROOT", cmd->query->relative_root);
  } else {
    w_envp_unset(cmd->envht, "WATCHMAN_RELATIVE_ROOT");
  }

  // Compute args
  auto args = json_deep_copy(cmd->command);

  if (cmd->append_files) {
    // Measure how much space the base args take up
    for (i = 0; i < json_array_size(args); i++) {
      const char *ele = json_string_value(json_array_get(args, i));

      argspace_remaining -= strlen(ele) + 1 + sizeof(char*);
    }

    // Dry run with env to compute space
    envp = w_envp_make_from_ht(cmd->envht, &env_size);
    free(envp);
    envp = NULL;
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

  argv = w_argv_copy_from_json(args, 0);
  args = nullptr;

  w_envp_set_bool(cmd->envht, "WATCHMAN_FILES_OVERFLOW", file_overflow);

  envp = w_envp_make_from_ht(cmd->envht, &env_size);

  posix_spawnattr_init(&attr);
#ifndef _WIN32
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
#endif
  posix_spawnattr_setflags(&attr,
      POSIX_SPAWN_SETSIGMASK|
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
      // Darwin: close everything except what we put in file actions
      POSIX_SPAWN_CLOEXEC_DEFAULT|
#endif
      POSIX_SPAWN_SETPGROUP);

  posix_spawn_file_actions_init(&actions);

#ifndef _WIN32
  posix_spawn_file_actions_adddup2(&actions, w_stm_fileno(stdin_file),
      STDIN_FILENO);
#else
  posix_spawn_file_actions_adddup2_handle_np(&actions,
      w_stm_handle(stdin_file), STDIN_FILENO);
#endif
  if (cmd->stdout_name) {
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
        cmd->stdout_name, cmd->stdout_flags, 0666);
  } else {
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDOUT_FILENO);
  }

  if (cmd->stderr_name) {
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
        cmd->stderr_name, cmd->stderr_flags, 0666);
  } else {
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
  }

  // Figure out the appropriate cwd
  {
    const char *cwd = NULL;
    working_dir = NULL;

    if (cmd->query->relative_root) {
      working_dir = cmd->query->relative_root;
    } else {
      working_dir = root->root_path;
    }
    w_string_addref(working_dir);

    json_unpack(cmd->definition, "{s:s}", "chdir", &cwd);
    if (cwd) {
      w_string_t *cwd_str = w_string_new_typed(cwd, W_STRING_BYTE);

      if (w_is_path_absolute_cstr(cwd)) {
        w_string_delref(working_dir);
        working_dir = cwd_str;
      } else {
        w_string_t *joined;

        joined = w_string_path_cat(working_dir, cwd_str);
        w_string_delref(cwd_str);
        w_string_delref(working_dir);

        working_dir = joined;
      }
    }

    w_log(W_LOG_DBG, "using %.*s for working dir\n", working_dir->len,
          working_dir->buf);
  }

#ifndef _WIN32
  // This mutex is present to avoid fighting over the cwd when multiple
  // triggers run at the same time.  It doesn't coordinate with all
  // possible chdir() calls, but this is the only place that we do this
  // in the watchman server process.
  static std::mutex cwdMutex;
  {
    std::unique_lock<std::mutex> lock(cwdMutex);
    ignore_result(chdir(working_dir->buf));
#else
    posix_spawnattr_setcwd_np(&attr, working_dir->buf);
#endif
    w_string_delref(working_dir);
    working_dir = nullptr;

    ret =
        posix_spawnp(&cmd->current_proc, argv[0], &actions, &attr, argv, envp);
    if (ret != 0) {
      // On Darwin (at least), posix_spawn can fail but will still populate the
      // pid.  Since we use the pid to gate future spawns, we need to ensure
      // that we clear out the pid on failure, otherwise the trigger would be
      // effectively disabled for the rest of the watch lifetime
      cmd->current_proc = 0;
    }
#ifndef _WIN32
    ignore_result(chdir("/"));
  }
#endif

  // If failed, we want to make sure we log enough info to figure out why
  result_log_level = res == 0 ? W_LOG_DBG : W_LOG_ERR;

  w_log(result_log_level, "posix_spawnp: %s\n", cmd->triggername.c_str());
  for (i = 0; argv[i]; i++) {
    w_log(result_log_level, "argv[%d] %s\n", i, argv[i]);
  }
  for (i = 0; envp[i]; i++) {
    w_log(result_log_level, "envp[%d] %s\n", i, envp[i]);
  }

  w_log(
      result_log_level,
      "trigger %s:%s pid=%d ret=%d %s\n",
      root->root_path.c_str(),
      cmd->triggername.c_str(),
      (int)cmd->current_proc,
      ret,
      strerror(ret));

  free(argv);
  free(envp);

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);

  if (stdin_file) {
    w_stm_close(stdin_file);
  }
}

bool watchman_trigger_command::maybeSpawn(unlocked_watchman_root* unlocked) {
  read_locked_watchman_root lock;
  bool didRun = false;

  // If it looks like we're in a repo undergoing a rebase or
  // other similar operation, we want to defer triggers until
  // things settle down
  if (unlocked->root->inner.view->isVCSOperationInProgress()) {
    w_log(W_LOG_DBG, "deferring triggers until VCS operations complete\n");
    return didRun;
  }

  w_root_read_lock(unlocked, "trigger assess", &lock);
  {
    w_query_res res;
    auto since_spec = query->since_spec.get();

    if (since_spec && since_spec->tag == w_cs_clock) {
      w_log(
          W_LOG_DBG,
          "running trigger \"%s\" rules! since %" PRIu32 "\n",
          triggername.c_str(),
          since_spec->clock.ticks);
    } else {
      w_log(W_LOG_DBG, "running trigger \"%s\" rules!\n", triggername.c_str());
    }

    // Triggers never need to sync explicitly; we are only dispatched
    // at settle points which are by definition sync'd to the present time
    query->sync_timeout = std::chrono::milliseconds(0);
    watchman::log(watchman::DBG, "assessing trigger ", triggername, "\n");
    if (!w_query_execute_locked(query.get(), &lock, &res, time_generator)) {
      watchman::log(
          watchman::ERR,
          "error running trigger \"",
          triggername,
          "\" query: ",
          res.errmsg,
          "\n");
      goto done;
    }

    watchman::log(
        watchman::DBG,
        "trigger \"",
        triggername,
        "\" generated ",
        res.resultsArray.array().size(),
        " results\n");

    // create a new spec that will be used the next time
    auto saved_spec = std::move(query->since_spec);
    query->since_spec = w_clockspec_new_clock(res.root_number, res.ticks);

    watchman::log(
        watchman::DBG,
        "updating trigger \"",
        triggername,
        "\" use ",
        res.ticks,
        " ticks next time\n");

    if (!res.resultsArray.array().empty()) {
      // transitional const_cast until we remove read_locked refs
      didRun = true;
      spawn_command(
          const_cast<w_root_t*>(lock.root), this, &res, saved_spec.get());
    }
  }
done:
  w_root_read_unlock(&lock, unlocked);
  return didRun;
}

/* vim:ts=2:sw=2:et:
 */
