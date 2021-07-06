/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/Synchronized.h>
#include <condition_variable>
#include <thread>
#include "watchman/ChildProcess.h"
#include "watchman/Logging.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/watchman.h"
#include "watchman/watchman_perf.h"
#include "watchman/watchman_root.h"
#include "watchman/watchman_system.h"
#include "watchman/watchman_time.h"

namespace watchman {
namespace {
class PerfLogThread {
  struct State {
    explicit State(bool start) : running(start) {}

    bool running;
    json_ref samples;
  };

  folly::Synchronized<State, std::mutex> state_;
  std::thread thread_;
  std::condition_variable cond_;

  void loop() noexcept;

 public:
  explicit PerfLogThread(bool start) : state_(folly::in_place, start) {
    if (start) {
      thread_ = std::thread([this] { loop(); });
    }
  }

  ~PerfLogThread() {
    stop();
  }

  void stop() {
    {
      auto state = state_.lock();
      if (!state->running) {
        return;
      }
      state->running = false;
    }
    cond_.notify_all();
    thread_.join();
  }

  void addSample(json_ref&& sample) {
    auto wlock = state_.lock();
    if (!wlock->samples) {
      wlock->samples = json_array();
    }
    json_array_append_new(wlock->samples, std::move(sample));
    cond_.notify_one();
  }
};

PerfLogThread& getPerfThread(bool start = true) {
  // Get the perf logging thread, starting it on the first call.
  // Meyer's singleton!
  static PerfLogThread perfThread(start);
  return perfThread;
}
} // namespace

void processSamples(
    size_t argv_limit,
    size_t maximum_batch_size,
    json_ref samples,
    std::function<void(std::vector<std::string>)> command_line,
    std::function<void(std::string)> single_large_sample) {
  while (json_array_size(samples) > 0) {
    std::string encoded_sample = json_dumps(json_array_get(samples, 0), 0);
    json_array_remove(samples, 0);

    if (encoded_sample.size() > argv_limit) {
      single_large_sample(std::move(encoded_sample));
    } else {
      std::vector<std::string> args;
      args.push_back(std::move(encoded_sample));
      size_t arg_size = encoded_sample.size() + 1;

      while (args.size() < maximum_batch_size && json_array_size(samples) > 0) {
        encoded_sample = json_dumps(json_array_get(samples, 0), 0);
        if (arg_size + encoded_sample.size() + 1 > argv_limit) {
          break;
        }
        json_array_remove(samples, 0);
        arg_size += encoded_sample.size() + 1;
        args.push_back(std::move(encoded_sample));
      }
      command_line(std::move(args));
    }
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
        if (thresh.isNumber()) {
          wall_time_elapsed_thresh = json_number_value(thresh);
        } else {
          wall_time_elapsed_thresh = json_number_value(
              thresh.get_default(description, json_real(0.0)));
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
  meta_data.set(key, std::move(val));
}

void watchman_perf_sample::add_root_meta(
    const std::shared_ptr<watchman_root>& root) {
  // Note: if the root lock isn't held, we may read inaccurate numbers for
  // some of these properties.  We're ok with that, and don't want to force
  // the root lock to be re-acquired just for this.
  auto meta = json_object(
      {{"path", w_string_to_json(root->root_path)},
       {"recrawl_count", json_integer(root->recrawlInfo.rlock()->recrawlCount)},
       {"case_sensitive",
        json_boolean(root->case_sensitive == CaseSensitivity::CaseSensitive)}});

  // During recrawl, the view may be re-assigned.  Protect against
  // reading a nullptr.
  auto view = root->view();
  if (view) {
    meta.set({{"watcher", w_string_to_json(view->getName())}});
  }

  add_meta("root", std::move(meta));
}

void watchman_perf_sample::set_wall_time_thresh(double thresh) {
  wall_time_elapsed_thresh = thresh;
}

void watchman_perf_sample::force_log() {
  will_log = true;
}

void PerfLogThread::loop() noexcept {
  json_ref samples;
  json_ref perf_cmd;
  int64_t sample_batch;

  w_set_thread_name("perflog");

  auto stateDir = w_string_piece(watchman_state_file).dirName().asWString();

  perf_cmd = cfg_get_json("perf_logger_command");
  if (perf_cmd.isString()) {
    perf_cmd = json_array({perf_cmd});
  }
  if (!perf_cmd.isArray()) {
    logf(
        FATAL,
        "perf_logger_command must be either a string or an array of strings\n");
  }

  sample_batch = cfg_get_int("perf_logger_command_max_samples_per_call", 4);

  while (true) {
    {
      auto state = state_.lock();
      while (true) {
        if (state->samples) {
          // We found samples to process
          break;
        }
        if (!state->running) {
          // No samples remaining, and we have been asked to quit.
          return;
        }
        cond_.wait(state.as_lock());
      }

      samples = nullptr;
      std::swap(samples, state->samples);
    }

    if (samples) {
      // Hack: Divide by two because this limit includes environment variables
      // and perf_cmd.
      // It's possible to compute this correctly on every platform given the
      // current environment and any specified environment variables, but it's
      // fine to be conservative here.
      const size_t argv_limit = ChildProcess::getArgMax() / 2;

      processSamples(
          argv_limit,
          sample_batch,
          samples,
          [&](std::vector<std::string> sample_args) {
            std::vector<w_string_piece> cmd;
            cmd.reserve(perf_cmd.array().size() + sample_args.size());

            for (auto& c : perf_cmd.array()) {
              cmd.push_back(json_to_w_string(c));
            }
            for (auto& sample : sample_args) {
              cmd.push_back(sample);
            }

            ChildProcess::Options opts;
            opts.environment().set(
                {{"WATCHMAN_STATE_DIR", stateDir},
                 {"WATCHMAN_SOCK", get_sock_name_legacy()}});
            opts.open(STDIN_FILENO, "/dev/null", O_RDONLY, 0666);
            opts.open(STDOUT_FILENO, "/dev/null", O_WRONLY, 0666);
            opts.open(STDERR_FILENO, "/dev/null", O_WRONLY, 0666);

            try {
              ChildProcess proc(cmd, std::move(opts));
              proc.wait();
            } catch (const std::exception& exc) {
              watchman::log(
                  watchman::ERR,
                  "failed to spawn perf logger: ",
                  exc.what(),
                  "\n");
            }
          },
          [&](std::string sample_stdin) {
            ChildProcess::Options opts;
            opts.environment().set(
                {{"WATCHMAN_STATE_DIR", stateDir},
                 {"WATCHMAN_SOCK", get_sock_name_legacy()}});
            opts.pipeStdin();
            opts.open(STDOUT_FILENO, "/dev/null", O_WRONLY, 0666);
            opts.open(STDERR_FILENO, "/dev/null", O_WRONLY, 0666);

            try {
              ChildProcess proc({perf_cmd}, std::move(opts));

              auto stdinPipe = proc.takeStdin();

              const char* data = sample_stdin.data();
              size_t size = sample_stdin.size();

              size_t total_written = 0;
              while (total_written < sample_stdin.size()) {
                auto result = stdinPipe->write.write(data, size);
                result.throwIfError();
                auto written = result.value();
                data += written;
                size -= written;
                total_written += written;
              }

              // close stdin to allow the process to terminate
              stdinPipe.reset();
              proc.wait();
            } catch (const std::exception& exc) {
              watchman::log(
                  watchman::ERR,
                  "failed to spawn perf logger: ",
                  exc.what(),
                  "\n");
            }
          });
    }
  }
}

void watchman_perf_sample::log() {
  if (!will_log) {
    return;
  }

  // Assemble a perf blob
  auto info = json_object(
      {{"description", typed_string_to_json(description)},
       {"meta", meta_data},
       {"pid", json_integer(::getpid())},
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
#endif // HAVE_SYS_RESOURCE_H
#undef ADDTV

  // Log to the log file
  auto dumped = json_dumps(info, 0);
  watchman::log(watchman::ERR, "PERF: ", dumped, "\n");

  if (!cfg_get_json("perf_logger_command")) {
    return;
  }

  // Send this to our logging thread for async processing
  auto& perfThread = getPerfThread();
  perfThread.addSample(std::move(info));
}

void perf_shutdown() {
  getPerfThread(false).stop();
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
