/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <array>
#include <limits>
#include <sstream>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/ThreadLocal.h>
#include <folly/system/ThreadName.h>
#include "Logging.h"

using namespace watchman;

int log_level = LogLevel::ERR;
static folly::ThreadLocal<folly::Optional<std::string>> threadName;
static constexpr size_t kMaxFrames = 64;

std::string log_name;

namespace {
template <typename String>
void write_stderr(const String& str) {
  w_string_piece piece = str;
  ignore_result(write(STDERR_FILENO, piece.data(), piece.size()));
}

template <typename String, typename... Strings>
void write_stderr(const String& str, Strings&&... strings) {
  write_stderr(str);
  write_stderr(strings...);
}
} // namespace

static void log_stack_trace(void) {
#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  std::array<void*, kMaxFrames> array;
  size_t size;
  char** strings;
  size_t i;

  size = backtrace(array.data(), array.size());
  strings = backtrace_symbols(array.data(), size);

  write_stderr("Fatal error detected at:\n");

  for (i = 0; i < size; i++) {
    write_stderr(strings[i], "\n");
  }

  free(strings);
#endif
}

namespace watchman {

namespace {
struct levelMaps {
  // Actually a map of LogLevel, w_string, but it is relatively high friction
  // to define the hasher for an enum key :-p
  std::unordered_map<int, w_string> levelToLabel;
  std::unordered_map<w_string, enum LogLevel> labelToLevel;

  levelMaps()
      : levelToLabel{{ABORT, "abort"},
                     {FATAL, "fatal"},
                     {ERR, "error"},
                     {OFF, "off"},
                     {DBG, "debug"}} {
    // Create the reverse map
    for (auto& it : levelToLabel) {
      labelToLevel.insert(
          std::make_pair(it.second, static_cast<enum LogLevel>(it.first)));
    }
  }
};

// Meyers singleton for holding the log level maps
levelMaps& getLevelMaps() {
  static levelMaps maps;
  return maps;
}

} // namespace

const w_string& logLevelToLabel(enum LogLevel level) {
  return getLevelMaps().levelToLabel.at(static_cast<int>(level));
}

enum LogLevel logLabelToLevel(const w_string& label) {
  return getLevelMaps().labelToLevel.at(label);
}

Log::Log()
    : errorPub_(std::make_shared<Publisher>()),
      debugPub_(std::make_shared<Publisher>()) {
  setStdErrLoggingLevel(ERR);
}

Log& getLog() {
  static Log log;
  return log;
}

char* Log::timeString(char* buf, size_t bufsize, timeval tv) {
  struct tm tm;
#ifdef _WIN32
  time_t seconds = (time_t)tv.tv_sec;
  tm = *localtime(&seconds);
#else
  localtime_r(&tv.tv_sec, &tm);
#endif

  char timebuf[64];
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);
  snprintf(buf, bufsize, "%s,%03d", timebuf, (int)tv.tv_usec / 1000);
  return buf;
}

char* Log::currentTimeString(char* buf, size_t bufsize) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return timeString(buf, bufsize, tv);
}

const char* Log::setThreadName(std::string&& name) {
  folly::setThreadName(name);
  threadName->assign(name);
  return threadName->value().c_str();
}

const char* Log::getThreadName() {
  if (!threadName->hasValue()) {
    auto name = folly::getCurrentThreadName();
    if (name.hasValue()) {
      threadName->assign(name);
    } else {
      std::stringstream ss;
      ss << std::this_thread::get_id();
      threadName->assign(ss.str());
    }
  }
  return threadName->value().c_str();
}

void Log::setStdErrLoggingLevel(enum LogLevel level) {
  auto notify = [this]() { doLogToStdErr(); };
  switch (level) {
    case OFF:
      errorSub_.reset();
      debugSub_.reset();
      return;
    case DBG:
      if (!debugSub_) {
        debugSub_ = debugPub_->subscribe(notify);
      }
      if (!errorSub_) {
        errorSub_ = errorPub_->subscribe(notify);
      }
      return;
    default:
      debugSub_.reset();
      if (!errorSub_) {
        errorSub_ = errorPub_->subscribe(notify);
      }
      return;
  }
}

void Log::doLogToStdErr() {
  std::vector<std::shared_ptr<const watchman::Publisher::Item>> items;

  {
    std::lock_guard<std::mutex> lock(stdErrPrintMutex_);
    getPending(items, errorSub_, debugSub_);
  }

  bool doFatal = false;
  bool doAbort = false;
  static w_string kFatal("fatal");
  static w_string kAbort("abort");

  for (auto& item : items) {
    auto& log = json_to_w_string(item->payload.get("log"));
    ignore_result(write(STDERR_FILENO, log.data(), log.size()));

    auto level = json_to_w_string(item->payload.get("level"));
    if (level == kFatal) {
      doFatal = true;
    } else if (level == kAbort) {
      doAbort = true;
    }
  }

  if (doFatal || doAbort) {
    log_stack_trace();
    if (doAbort) {
      abort();
    } else {
      _exit(1);
    }
  }
}

} // namespace watchman

#ifdef _WIN32
static LONG WINAPI exception_filter(LPEXCEPTION_POINTERS excep) {
  std::array<void*, kMaxFrames> array;
  size_t size;
  char** strings;
  size_t i;
  char timebuf[64];

  size = backtrace_from_exception(excep, array.data(), array.size());
  strings = backtrace_symbols(array.data(), size);

  write_stderr(
      watchman::Log::currentTimeString(timebuf, sizeof(timebuf)),
      ": [",
      watchman::Log::getThreadName(),
      "] Unhandled win32 exception.  Fatal error detected at:\n");

  for (i = 0; i < size; i++) {
    write_stderr(strings[i], "\n");
  }
  free(strings);

  write_stderr("the stack trace for the exception filter call is:\n");
  size = backtrace(array.data(), array.size());
  strings = backtrace_symbols(array.data(), size);
  for (i = 0; i < size; i++) {
    write_stderr(strings[i], "\n");
  }
  free(strings);

  // Terminate the process.
  // msvcrt abort() ultimately calls exit(3), so we shortcut that.
  exit(3);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif
