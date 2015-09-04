/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Maps pid => root
static w_ht_t *running_kids = NULL;
static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;

static void spawn_command(w_root_t *root,
  struct watchman_trigger_command *cmd,
  w_query_res *res,
  struct w_clockspec *since_spec);

// Caller must hold spawn_lock
static w_root_t *lookup_running_pid(pid_t pid)
{
  if (!running_kids) {
    return NULL;
  }

  return w_ht_val_ptr(w_ht_get(running_kids, pid));
}

// Caller must hold spawn_lock
static void delete_running_pid(pid_t pid)
{
  if (!running_kids) {
    return;
  }
  w_ht_del(running_kids, pid);
}

// Caller must hold spawn_lock
static void insert_running_pid(pid_t pid, w_root_t *root)
{
  if (!running_kids) {
    running_kids = w_ht_new(2, NULL);
  }
  w_ht_set(running_kids, pid, w_ht_ptr_val(root));
}

void w_mark_dead(pid_t pid)
{
  w_root_t *root = NULL;
  w_ht_iter_t iter;

  pthread_mutex_lock(&spawn_lock);
  root = lookup_running_pid(pid);
  if (!root) {
    pthread_mutex_unlock(&spawn_lock);
    return;
  }
  delete_running_pid(pid);
  pthread_mutex_unlock(&spawn_lock);

  w_log(W_LOG_DBG, "mark_dead: %.*s child pid %d\n",
      root->root_path->len, root->root_path->buf, (int)pid);

  /* now walk the cmds and try to find our match */
  w_root_lock(root);

  /* walk the list of triggers, and run their rules */
  if (w_ht_first(root->commands, &iter)) do {
    struct watchman_trigger_command *cmd;

    cmd = w_ht_val_ptr(iter.value);
    if (cmd->current_proc != pid) {
      w_log(W_LOG_DBG, "mark_dead: is [%.*s] %d == %d\n",
          cmd->triggername->len, cmd->triggername->buf,
          (int)cmd->current_proc, (int)pid);
      continue;
    }

    /* first mark the process as dead */
    cmd->current_proc = 0;
    if (root->cancelled) {
      w_log(W_LOG_DBG, "mark_dead: root was cancelled\n");
      break;
    }

    w_assess_trigger(root, cmd);
    break;
  } while (w_ht_next(root->commands, &iter));

  w_root_unlock(root);
  w_root_delref(root);
}

static w_stm_t prepare_stdin(
  struct watchman_trigger_command *cmd,
  w_query_res *res)
{
  uint32_t n_files;
  char stdin_file_name[WATCHMAN_NAME_MAX];
  w_stm_t stdin_file = NULL;

  if (cmd->stdin_style == input_dev_null) {
    return w_stm_open("/dev/null", O_RDONLY|O_CLOEXEC);
  }

  n_files = res->num_results;

  if (cmd->max_files_stdin > 0) {
    n_files = MIN(cmd->max_files_stdin, n_files);
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
        json_t *file_list;

        if (!w_json_buffer_init(&buffer)) {
          w_log(W_LOG_ERR, "failed to init json buffer\n");
          w_stm_close(stdin_file);
          return NULL;
        }

        file_list = w_query_results_to_json(&cmd->field_list,
            n_files, res->results);
        w_log(W_LOG_ERR, "input_json: sending json object to stm\n");
        if (!w_json_buffer_write(&buffer, stdin_file, file_list, 0)) {
          w_log(W_LOG_ERR,
              "input_json: failed to write json data to stream: %s\n",
              strerror(errno));
          w_stm_close(stdin_file);
          return NULL;
        }
        w_json_buffer_free(&buffer);
        json_decref(file_list);
        break;
      }
    case input_name_list:
      {
        uint32_t i;

        for (i = 0; i < n_files; i++) {
          if (w_stm_write(stdin_file, res->results[i].relname->buf,
              res->results[i].relname->len) != (int)res->results[i].relname->len
              || w_stm_write(stdin_file, "\n", 1) != 1) {
            w_log(W_LOG_ERR,
              "write failure while producing trigger stdin: %s\n",
              strerror(errno));
            w_stm_close(stdin_file);
            return NULL;
          }
        }
        break;
      }
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
  json_t *args;
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

  stdin_file = prepare_stdin(cmd, res);
  if (!stdin_file) {
    w_log(W_LOG_ERR, "trigger %.*s:%s %s\n",
        (int)root->root_path->len,
        root->root_path->buf,
        cmd->triggername->buf, strerror(errno));
    return;
  }

  // Assumption: that only one thread will be executing on a given
  // cmd instance so that mutation of cmd->envht is safe.
  // This is guaranteed in the current architecture.

  if (cmd->max_files_stdin > 0 && res->num_results > cmd->max_files_stdin) {
    file_overflow = true;
  }

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
  args = json_deep_copy(cmd->command);

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

    for (i = 0; i < res->num_results; i++) {
      // also: NUL terminator and entry in argv
      uint32_t size = res->results[i].relname->len + 1 + sizeof(char*);

      if (argspace_remaining < size) {
        file_overflow = true;
        break;
      }
      argspace_remaining -= size;

      json_array_append_new(
        args,
        json_string_nocheck(res->results[i].relname->buf)
      );
    }
  }

  argv = w_argv_copy_from_json(args, 0);
  json_decref(args);
  args = NULL;

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
      w_string_t *cwd_str = w_string_new(cwd);

      if (w_is_path_absolute(cwd)) {
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

    w_log(W_LOG_DBG, "using %.*s for working dir\n", working_dir->len, working_dir->buf);
  }

  pthread_mutex_lock(&spawn_lock);
#ifndef _WIN32
  ignore_result(chdir(working_dir->buf));
#else
  posix_spawnattr_setcwd_np(&attr, working_dir->buf);
#endif
  w_string_delref(working_dir);
  working_dir = NULL;

  ret = posix_spawnp(&cmd->current_proc, argv[0], &actions, &attr, argv, envp);
  if (ret == 0) {
    w_root_addref(root);
    insert_running_pid(cmd->current_proc, root);
  } else {
    // On Darwin (at least), posix_spawn can fail but will still populate the
    // pid.  Since we use the pid to gate future spawns, we need to ensure
    // that we clear out the pid on failure, otherwise the trigger would be
    // effectively disabled for the rest of the watch lifetime
    cmd->current_proc = 0;
  }
#ifndef _WIN32
  ignore_result(chdir("/"));
#endif
  pthread_mutex_unlock(&spawn_lock);

  // If failed, we want to make sure we log enough info to figure out why
  result_log_level = res == 0 ? W_LOG_DBG : W_LOG_ERR;

  w_log(result_log_level, "posix_spawnp: %s\n", cmd->triggername->buf);
  for (i = 0; argv[i]; i++) {
    w_log(result_log_level, "argv[%d] %s\n", i, argv[i]);
  }
  for (i = 0; envp[i]; i++) {
    w_log(result_log_level, "envp[%d] %s\n", i, envp[i]);
  }

  w_log(result_log_level, "trigger %.*s:%s pid=%d ret=%d %s\n",
      (int)root->root_path->len,
      root->root_path->buf,
      cmd->triggername->buf, (int)cmd->current_proc, ret, strerror(ret));

  free(argv);
  free(envp);

  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);

  if (stdin_file) {
    w_stm_close(stdin_file);
  }
}

static bool trigger_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx,
    void *gendata)
{
  struct watchman_file *f;
  struct watchman_trigger_command *cmd = gendata;

  w_log(W_LOG_DBG, "assessing trigger %s %p\n",
      cmd->triggername->buf, cmd);

  // Walk back in time until we hit the boundary
  for (f = root->latest_file; f; f = f->next) {
    if (ctx->since.is_timestamp &&
        w_timeval_compare(f->otime.tv, ctx->since.timestamp) < 0) {
      break;
    }
    if (!ctx->since.is_timestamp &&
        f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      return false;
    }
  }

  return true;
}

/* must be called with root locked */
void w_assess_trigger(w_root_t *root, struct watchman_trigger_command *cmd)
{
  w_query_res res;
  struct w_clockspec *since_spec = cmd->query->since_spec;

  if (since_spec && since_spec->tag == w_cs_clock) {
    w_log(W_LOG_DBG, "running trigger \"%s\" rules! since %" PRIu32 "\n",
        cmd->triggername->buf,
        since_spec->clock.ticks);
  } else {
    w_log(W_LOG_DBG, "running trigger \"%s\" rules!\n",
        cmd->triggername->buf);
  }

  // Triggers never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  cmd->query->sync_timeout = 0;
  if (!w_query_execute(cmd->query, root, &res, trigger_generator, cmd)) {
    w_log(W_LOG_ERR, "error running trigger \"%s\" query: %s",
        cmd->triggername->buf, res.errmsg);
    w_query_result_free(&res);
    return;
  }

  w_log(W_LOG_DBG, "trigger \"%s\" generated %" PRIu32 " results\n",
      cmd->triggername->buf, res.num_results);

  // create a new spec that will be used the next time
  cmd->query->since_spec = w_clockspec_new_clock(res.root_number, res.ticks);

  w_log(W_LOG_DBG, "updating trigger \"%s\" use %" PRIu32 " ticks next time\n",
      cmd->triggername->buf, res.ticks);

  if (res.num_results) {
    spawn_command(root, cmd, &res, since_spec);
  }

  if (since_spec) {
    w_clockspec_free(since_spec);
    since_spec = NULL;
  }

  w_query_result_free(&res);
}

/* vim:ts=2:sw=2:et:
 */
