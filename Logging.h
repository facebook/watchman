/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "PubSub.h"
#include "watchman_preprocessor.h"
#include "watchman_string.h"

namespace watchman {

// Coupled with the defines in watchman_log.h
enum LogLevel { FATAL = -1, OFF = 0, ERR = 1, DBG = 2 };

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
  static const char* getThreadName();

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

  inline void logVPrintf(enum LogLevel level, const char* fmt, va_list ap) {
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
                          w_string::vprintf(fmt, ap)))},
                     {"unilateral", json_true()},
                     {"level", typed_string_to_json(logLevelToLabel(level))}});

    pub.enqueue(std::move(payload));
  }

  inline void logPrintf(
      enum LogLevel level,
      WATCHMAN_FMT_STRING(const char* fmt),
      ...) WATCHMAN_FMT_ATTR(3, 4) {
    va_list ap;
    va_start(ap, fmt);
    logVPrintf(level, fmt, ap);
    va_end(ap);
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
void logPrintf(enum LogLevel level, Args&&... args) {
  getLog().logPrintf(level, std::forward<Args>(args)...);
}
}
