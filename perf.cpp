/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include <condition_variable>
#include <thread>
#include "ChildProcess.h"
#include "watchman.h"
#include "watchman_perf.h"
#include "watchman_synchronized.h"

using watchman::ChildProcess;
using Options = ChildProcess::Options;
using Environment = ChildProcess::Environment;
using CaseSensitivity = watchman::CaseSensitivity;

namespace {
class PerfLogThread {
  watchman::Synchronized<json_ref, std::mutex> samples_;
  std::thread thread_;
  std::condition_variable cond_;

  void loop();

 public:
  PerfLogThread() : thread_([this]() { loop(); }) {}

  ~PerfLogThread() {
    cond_.notify_all();
    thread_.join();
  }

  void addSample(json_ref&& sample) {
    auto wlock = samples_.wlock();
    if (!*wlock) {
      *wlock = json_array();
    }
    json_array_append_new(*wlock, std::move(sample));
    cond_.notify_one();
  }
};

PerfLogThread& getPerfThread() {
  // Get the perf logging thread, starting it on the first call.
  // Meyer's singleton!
  static PerfLogThread perfThread;
  return perfThread;
}
}

watchman_perf_sample::watchman_perf_sample(const char* description)
    : description(description) {
  gettimeofday(&time_begin, nullptr);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_begin);
#endif
}

bool watchman_perf_sample::finish() {
  gettimeofday(&time_end, nullptr);
  w_timeval_sub(time_end, time_begin, &duration);
#ifdef HAVE_SYS_RESOURCE_H
  getrusage(RUSAGE_SELF, &usage_end);

  // Compute the delta for the usage
  w_timeval_sub(usage_end.ru_utime, usage_begin.ru_utime, &usage.ru_utime);
  w_timeval_sub(usage_end.ru_stime, usage_begin.ru_stime, &usage.ru_stime);

#define DIFFU(n) usage.n = usage_end.n - usage_begin.n
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

  if (!will_log) {
    if (wall_time_elapsed_thresh == 0) {
      auto thresh = cfg_get_json("perf_sampling_thresh");
      if (thresh) {
        if (json_is_number(thresh)) {
          wall_time_elapsed_thresh = json_number_value(thresh);
        } else {
          json_unpack(thresh, "{s:f}", description, &wall_time_elapsed_thresh);
        }
      }
    }

    if (wall_time_elapsed_thresh > 0 &&
        w_timeval_diff(time_begin, time_end) > wall_time_elapsed_thresh) {
      will_log = true;
    }
  }

  return will_log;
}

void watchman_perf_sample::add_meta(const char* key, json_ref&& val) {
  if (!meta_data) {
    meta_data = json_object();
  }
  meta_data.set(key, std::move(val));
}

void watchman_perf_sample::add_root_meta(
    const std::shared_ptr<w_root_t>& root) {
  // Note: if the root lock isn't held, we may read inaccurate numbers for
  // some of these properties.  We're ok with that, and don't want to force
  // the root lock to be re-acquired just for this.
  auto meta = json_object(
      {{"path", w_string_to_json(root->root_path)},
       {"recrawl_count", json_integer(root->recrawlInfo.rlock()->recrawlCount)},
       {"case_sensitive", json_boolean(root->case_sensitive == CaseSensitivity::CaseSensitive)}});

  // During recrawl, the view may be re-assigned.  Protect against
  // reading a nullptr.
  auto view = root->view();
  if (view) {
    auto position = view->getMostRecentRootNumberAndTickValue();
    meta.set({{"number", json_integer(position.rootNumber)},
              {"ticks", json_integer(position.ticks)},
              {"watcher", w_string_to_json(view->getName())}});
  }

  add_meta("root", std::move(meta));
}

void watchman_perf_sample::set_wall_time_thresh(double thresh) {
  wall_time_elapsed_thresh = thresh;
}

void watchman_perf_sample::force_log() {
  will_log = true;
}

void PerfLogThread::loop() {
  json_ref samples;
  json_ref perf_cmd;
  int64_t sample_batch;

  w_set_thread_name("perflog");

  auto stateDir = w_string_piece(watchman_state_file).dirName().asWString();

  perf_cmd = cfg_get_json("perf_logger_command");
  if (json_is_string(perf_cmd)) {
    perf_cmd = json_array({perf_cmd});
  }
  if (!json_is_array(perf_cmd)) {
    w_log(
        W_LOG_FATAL,
        "perf_logger_command must be either a string or an array of strings\n");
  }

  sample_batch = cfg_get_int("perf_logger_command_max_samples_per_call", 4);

  while (!w_is_stopping()) {
    {
      auto wlock = samples_.wlock();
      if (!*wlock) {
        cond_.wait(wlock.getUniqueLock());
      }

      samples = nullptr;
      std::swap(samples, *wlock);
    }

    if (samples) {
      while (json_array_size(samples) > 0) {
        int i = 0;
        auto cmd = json_array();

        json_array_extend(cmd, perf_cmd);

        while (i < sample_batch && json_array_size(samples) > 0) {
          char *stringy = json_dumps(json_array_get(samples, 0), 0);
          if (stringy) {
            json_array_append_new(
                cmd, typed_string_to_json(stringy, W_STRING_MIXED));
            free(stringy);
          }
          json_array_remove(samples, 0);
          i++;
        }

        ChildProcess::Options opts;
        opts.environment().set({{"WATCHMAN_STATE_DIR", stateDir},
                                {"WATCHMAN_SOCK", get_sock_name()}});
        opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
        opts.open(STDOUT_FILENO, "/dev/null", O_WRONLY, 0666);
        opts.open(STDERR_FILENO, "/dev/null", O_WRONLY, 0666);

        try {
          ChildProcess proc(cmd, std::move(opts));
          proc.wait();
        } catch (const std::exception& exc) {
          watchman::log(
              watchman::ERR, "failed to spawn perf logger: ", exc.what(), "\n");
        }
      }
    }
  }
}

void watchman_perf_sample::log() {
  char *dumped = NULL;

  if (!will_log) {
    return;
  }

  // Assemble a perf blob
  auto info = json_object(
      {{"description", typed_string_to_json(description)},
       {"meta", meta_data},
       {"pid", json_integer(getpid())},
       {"version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE)}});

#ifdef WATCHMAN_BUILD_INFO
  info.set(
      "buildinfo", typed_string_to_json(WATCHMAN_BUILD_INFO, W_STRING_UNICODE));
#endif

#define ADDTV(name, tv) info.set(name, json_real(w_timeval_abs_seconds(tv)))
  ADDTV("elapsed_time", duration);
  ADDTV("start_time", time_begin);
#ifdef HAVE_SYS_RESOURCE_H
  ADDTV("user_time", usage.ru_utime);
  ADDTV("system_time", usage.ru_stime);
#define ADDU(n) info.set(#n, json_integer(usage.n))
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

  if (!cfg_get_json("perf_logger_command")) {
    return;
  }

  // Send this to our logging thread for async processing
  auto& perfThread = getPerfThread();
  perfThread.addSample(std::move(info));
}

/* vim:ts=2:sw=2:et:
 */
