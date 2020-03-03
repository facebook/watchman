/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_string.h"
#include "PubSub.h"
#include "watchman_preprocessor.h"

namespace watchman {

// Coupled with the defines in watchman_log.h
enum LogLevel { ABORT = -2, FATAL = -1, OFF = 0, ERR = 1, DBG = 2 };

const w_string& logLevelToLabel(enum LogLevel level);
enum LogLevel logLabelToLevel(const w_string& label);

class Log {
 public:
  std::shared_ptr<Publisher::Subscriber> subscribe(
      enum LogLevel level,
      Publisher::Notifier notify) {
    return levelToPub(level).subscribe(notify);
  }

  static char* currentTimeString(char* buf, size_t bufsize);
  static char* timeString(char* buf, size_t bufsize, timeval tv);
  static const char* getThreadName();
  static const char* setThreadName(std::string&& name);

  void setStdErrLoggingLevel(enum LogLevel level);

  // Build a string and log it
  template <typename... Args>
  void log(enum LogLevel level, Args&&... args) {
    auto& pub = levelToPub(level);

    // Avoid building the string if there are no subscribers
    if (!pub.hasSubscribers()) {
      return;
    }

    char timebuf[64];

    auto payload =
        json_object({{"log",
                      typed_string_to_json(w_string::build(
                          currentTimeString(timebuf, sizeof(timebuf)),
                          ": [",
                          getThreadName(),
                          "] ",
                          std::forward<Args>(args)...))},
                     {"unilateral", json_true()},
                     {"level", typed_string_to_json(logLevelToLabel(level))}});

    pub.enqueue(std::move(payload));
  }

  // Format a string and log it
  template <typename... Args>
  void logf(enum LogLevel level, fmt::string_view format_str, Args&&... args) {
    auto& pub = levelToPub(level);

    // Avoid building the string if there are no subscribers
    if (!pub.hasSubscribers()) {
      return;
    }

    char timebuf[64];

    auto payload = json_object(
        {{"log",
          typed_string_to_json(w_string::build(
              currentTimeString(timebuf, sizeof(timebuf)),
              ": [",
              getThreadName(),
              "] ",
              fmt::format(format_str, std::forward<Args>(args)...)))},
         {"unilateral", json_true()},
         {"level", typed_string_to_json(logLevelToLabel(level))}});

    pub.enqueue(std::move(payload));
  }

  Log();

 private:
  std::shared_ptr<Publisher> errorPub_;
  std::shared_ptr<Publisher> debugPub_;
  std::shared_ptr<Publisher::Subscriber> errorSub_;
  std::shared_ptr<Publisher::Subscriber> debugSub_;
  std::mutex stdErrPrintMutex_;

  inline Publisher& levelToPub(enum LogLevel level) {
    return level == DBG ? *debugPub_ : *errorPub_;
  }

  void doLogToStdErr();
};

// Get the logger singleton
Log& getLog();

template <typename... Args>
void log(enum LogLevel level, Args&&... args) {
  getLog().log(level, std::forward<Args>(args)...);
}

template <typename... Args>
void logf(enum LogLevel level, fmt::string_view format_str, Args&&... args) {
  getLog().logf(level, format_str, std::forward<Args>(args)...);
}

} // namespace watchman

template <typename... Args>
const char* w_set_thread_name(Args&&... args) {
  auto name = folly::to<std::string>(std::forward<Args>(args)...);
  return watchman::Log::setThreadName(std::move(name));
}

#define w_check(e, ...)                          \
  if (!(e)) {                                    \
    watchman::logf(                              \
        watchman::ERR,                           \
        "{}:{} failed assertion `{}'\n",         \
        __FILE__,                                \
        __LINE__,                                \
        #e);                                     \
    watchman::log(watchman::FATAL, __VA_ARGS__); \
  }

// Similar to assert(), but uses W_LOG_FATAL to log the stack trace
// before giving up the ghost
#ifdef NDEBUG
#define w_assert(e, ...) ((void)0)
#else
#define w_assert(e, ...) w_check(e, __VA_ARGS__)
#endif
