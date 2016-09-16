/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_perf.h"

static pthread_t perf_log_thr;
static pthread_mutex_t perf_log_lock = PTHREAD_MUTEX_INITIALIZER;
static json_t *perf_log_samples = NULL;
static pthread_cond_t perf_log_cond;
static bool perf_log_thread_started = false;

void w_perf_start(w_perf_t *perf, const char *description) {
  perf->root_path = NULL;
  perf->description = description;
  perf->meta_data = NULL;
  perf->will_log = false;
  perf->wall_time_elapsed_thresh = 0;

  gettimeofday(&perf->time_begin, NULL);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &perf->usage_begin);
#endif
}

void w_perf_destroy(w_perf_t *perf) {
  if (perf->root_path) {
    w_string_delref(perf->root_path);
  }
  if (perf->meta_data) {
    json_decref(perf->meta_data);
  }
  memset(perf, 0, sizeof(*perf));
}

bool w_perf_finish(w_perf_t *perf) {
  gettimeofday(&perf->time_end, NULL);
  w_timeval_sub(perf->time_end, perf->time_begin, &perf->duration);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &perf->usage_end);

  // Compute the delta for the usage
  w_timeval_sub(perf->usage_end.ru_utime, perf->usage_begin.ru_utime,
                &perf->usage.ru_utime);
  w_timeval_sub(perf->usage_end.ru_stime, perf->usage_begin.ru_stime,
                &perf->usage.ru_stime);

#define DIFFU(n) perf->usage.n = perf->usage_end.n - perf->usage_begin.n
  DIFFU(ru_maxrss);
  DIFFU(ru_ixrss);
  DIFFU(ru_idrss);
  DIFFU(ru_minflt);
  DIFFU(ru_majflt);
  DIFFU(ru_nswap);
  DIFFU(ru_inblock);
  DIFFU(ru_oublock);
  DIFFU(ru_msgsnd);
  DIFFU(ru_msgrcv);
  DIFFU(ru_nsignals);
  DIFFU(ru_nvcsw);
  DIFFU(ru_nivcsw);
#undef DIFFU
#endif

  if (!perf->will_log) {
    if (perf->wall_time_elapsed_thresh == 0) {
      json_t *thresh = cfg_get_json(NULL, "perf_sampling_thresh");
      if (thresh) {
        if (json_is_number(thresh)) {
          perf->wall_time_elapsed_thresh = json_number_value(thresh);
        } else {
          json_unpack(thresh, "{s:f}", perf->description,
                    &perf->wall_time_elapsed_thresh);
        }
      }
    }

    if (perf->wall_time_elapsed_thresh > 0 &&
        w_timeval_diff(perf->time_begin, perf->time_end) >
            perf->wall_time_elapsed_thresh) {
      perf->will_log = true;
    }
  }

  return perf->will_log;
}

void w_perf_add_meta(w_perf_t *perf, const char *key, json_t *val) {
  if (!perf->meta_data) {
    perf->meta_data = json_object();
  }
  set_prop(perf->meta_data, key, val);
}

void w_perf_add_root_meta(w_perf_t *perf, const w_root_t *root) {
  // Note: if the root lock isn't held, we may read inaccurate numbers for
  // some of these properties.  We're ok with that, and don't want to force
  // the root lock to be re-acquired just for this.

  // The funky comments at the end of the line force clang-format to keep the
  // elements on lines of their own
  w_perf_add_meta(
      perf,
      "root",
      json_pack(
          "{s:o, s:i, s:i, s:i, s:b, s:u}",
          "path",
          w_string_to_json(root->root_path),
          "recrawl_count",
          root->recrawl_count,
          "number",
          root->inner.number,
          "ticks",
          root->inner.ticks,
          "case_sensitive",
          root->case_sensitive,
          // there is potential to race with a concurrent w_root_init in some
          // recrawl scenarios in the test harness.  In those cases it is
          // possible that root->watcher_ops is briefly set to a NULL pointer.
          // Since the target of that pointer is always a structure with a
          // stable address, we can safely deal with reading a stale value, but
          // we do need to guard against a NULL pointer value.
          "watcher",
          root->watcher_ops ? root->watcher_ops->name : "<recrawling>"));
}

void w_perf_set_wall_time_thresh(w_perf_t *perf, double thresh) {
  perf->wall_time_elapsed_thresh = thresh;
}

void w_perf_force_log(w_perf_t *perf) {
  perf->will_log = true;
}

static void *perf_log_thread(void *unused) {
  json_t *samples = NULL;
  char **envp;
  json_t *perf_cmd;
  int64_t sample_batch;

  unused_parameter(unused);

  w_set_thread_name("perflog");

  // Prep some things that we'll need each time we run a command
  {
    uint32_t env_size;
    w_ht_t *envpht = w_envp_make_ht();
    char *statedir = dirname(strdup(watchman_state_file));
    w_envp_set_cstring(envpht, "WATCHMAN_STATE_DIR", statedir);
    w_envp_set_cstring(envpht, "WATCHMAN_SOCK", get_sock_name());
    envp = w_envp_make_from_ht(envpht, &env_size);
    w_ht_free(envpht);
  }

  perf_cmd = cfg_get_json(NULL, "perf_logger_command");
  if (json_is_string(perf_cmd)) {
    perf_cmd = json_pack("[O]", perf_cmd);
  }
  if (!json_is_array(perf_cmd)) {
    w_log(
        W_LOG_FATAL,
        "perf_logger_command must be either a string or an array of strings\n");
  }

  sample_batch =
      cfg_get_int(NULL, "perf_logger_command_max_samples_per_call", 4);

  while (true) {
    pthread_mutex_lock(&perf_log_lock);
    if (!perf_log_samples) {
      pthread_cond_wait(&perf_log_cond, &perf_log_lock);
    }
    samples = perf_log_samples;
    perf_log_samples = NULL;

    pthread_mutex_unlock(&perf_log_lock);

    if (samples) {
      while (json_array_size(samples) > 0) {
        int i = 0;
        json_t *cmd = json_array();
        posix_spawnattr_t attr;
        posix_spawn_file_actions_t actions;
        pid_t pid;
        char **argv = NULL;

        json_array_extend(cmd, perf_cmd);

        while (i < sample_batch && json_array_size(samples) > 0) {
          char *stringy = json_dumps(json_array_get(samples, 0), 0);
          if (stringy) {
            json_array_append(cmd,
                              typed_string_to_json(stringy, W_STRING_MIXED));
            free(stringy);
          }
          json_array_remove(samples, 0);
          i++;
        }

        argv = w_argv_copy_from_json(cmd, 0);
        if (!argv) {
          char *dumped = json_dumps(cmd, 0);
          w_log(W_LOG_FATAL, "error converting %s to an argv array\n", dumped);
        }

        posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0666);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                         O_WRONLY, 0666);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0666);

        if (posix_spawnp(&pid, argv[0], &actions, &attr, argv, envp) == 0) {
          // There's no sense waiting here, because w_reap_children is called
          // by the reaper thread.
        } else {
          int err = errno;
          w_log(W_LOG_ERR, "failed to spawn %s: %s\n", argv[0],
                strerror(err));
        }

        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);

        free(argv);
        json_decref(cmd);
      }
      json_decref(samples);
    }
  }

  return NULL;
}


void w_perf_log(w_perf_t *perf) {
  json_t *info;
  char *dumped = NULL;

  if (!perf->will_log) {
    return;
  }

  // Assemble a perf blob
  info = json_pack("{s:u, s:O, s:i, s:u}",           //
                   "description", perf->description, //
                   "meta", perf->meta_data,          //
                   "pid", getpid(),                  //
                   "version", PACKAGE_VERSION        //
                   );

#ifdef WATCHMAN_BUILD_INFO
  set_unicode_prop(info, "buildinfo", WATCHMAN_BUILD_INFO);
#endif

#define ADDTV(name, tv)                                                        \
  set_prop(info, name, json_real(w_timeval_abs_seconds(tv)))
  ADDTV("elapsed_time", perf->duration);
  ADDTV("start_time", perf->time_begin);
#ifdef HAVE_SYS_RESOURCE_H
  ADDTV("user_time", perf->usage.ru_utime);
  ADDTV("system_time", perf->usage.ru_stime);
#define ADDU(n) set_prop(info, #n, json_integer(perf->usage.n))
  ADDU(ru_maxrss);
  ADDU(ru_ixrss);
  ADDU(ru_idrss);
  ADDU(ru_minflt);
  ADDU(ru_majflt);
  ADDU(ru_nswap);
  ADDU(ru_inblock);
  ADDU(ru_oublock);
  ADDU(ru_msgsnd);
  ADDU(ru_msgrcv);
  ADDU(ru_nsignals);
  ADDU(ru_nvcsw);
  ADDU(ru_nivcsw);
#endif // HAVE_SYS_RESOURCE_H
#undef ADDU
#undef ADDTV

  // Log to the log file
  dumped = json_dumps(info, 0);
  w_log(W_LOG_ERR, "PERF: %s\n", dumped);
  free(dumped);

  if (!cfg_get_json(NULL, "perf_logger_command")) {
    json_decref(info);
    return;
  }

  // Send this to our logging thread for async processing

  pthread_mutex_lock(&perf_log_lock);
  if (!perf_log_thread_started) {
    pthread_cond_init(&perf_log_cond, NULL);
    pthread_create(&perf_log_thr, NULL, perf_log_thread, NULL);
    perf_log_thread_started = true;
  }

  if (!perf_log_samples) {
    perf_log_samples = json_array();
  }
  json_array_append_new(perf_log_samples, info);
  pthread_mutex_unlock(&perf_log_lock);

  pthread_cond_signal(&perf_log_cond);
}

/* vim:ts=2:sw=2:et:
 */
