/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "Logging.h"
#include <limits>

int log_level = W_LOG_ERR;
static pthread_key_t thread_name_key;

static void write_str_stderr(const char* str) {
  ignore_result(write(STDERR_FILENO, str, strlen(str)));
}

static void log_stack_trace(void) {
#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  void *array[24];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, sizeof(array)/sizeof(array[0]));
  strings = backtrace_symbols(array, size);

  write_str_stderr("Fatal error detected at:\n");

  for (i = 0; i < size; i++) {
    write_str_stderr(strings[i]);
    write_str_stderr("\n");
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
      : levelToLabel{{FATAL, "fatal"},
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

}

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

char* Log::currentTimeString(char* buf, size_t bufsize) {
  struct timeval tv;
  char timebuf[64];
  struct tm tm;

  gettimeofday(&tv, NULL);
#ifdef _WIN32
  tm = *localtime(&tv.tv_sec);
#else
  localtime_r(&tv.tv_sec, &tm);
#endif
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);

  snprintf(buf, bufsize, "%s,%03d", timebuf, (int)tv.tv_usec / 1000);

  return buf;
}

const char* Log::getThreadName() {
  auto name = (const char*)pthread_getspecific(thread_name_key);
  if (name) {
    return name;
  }
  return w_set_thread_name("%" PRIu32, (uint32_t)(uintptr_t)pthread_self());
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
  auto items = getPending(errorSub_, debugSub_);

  bool fatal = false;
  static w_string kFatal("fatal");

  for (auto& item : items) {
    auto& log = json_to_w_string(item->payload.get("log"));
    ignore_result(write(STDERR_FILENO, log.data(), log.size()));

    if (json_to_w_string(item->payload.get("level")) == kFatal) {
      fatal = true;
    }
  }

  if (fatal) {
    log_stack_trace();
    abort();
  }
}

} // namespace watchman

#ifndef _WIN32
static void crash_handler(int signo, siginfo_t *si, void *ucontext) {
  const char *reason = "";
  unused_parameter(ucontext);
  if (si) {
    switch (si->si_signo) {
      case SIGILL:
        switch (si->si_code) {
          case ILL_ILLOPC: reason = "illegal opcode"; break;
          case ILL_ILLOPN: reason = "illegal operand"; break;
          case ILL_ILLADR: reason = "illegal addressing mode"; break;
          case ILL_ILLTRP: reason = "illegal trap"; break;
          case ILL_PRVOPC: reason = "privileged opcode"; break;
          case ILL_PRVREG: reason = "privileged register"; break;
          case ILL_COPROC: reason = "co-processor error"; break;
          case ILL_BADSTK: reason = "internal stack error"; break;
        }
        break;
      case SIGFPE:
        switch (si->si_code) {
          case FPE_INTDIV: reason = "integer divide by zero"; break;
          case FPE_INTOVF: reason = "integer overflow"; break;
          case FPE_FLTDIV: reason = "floating point divide by zero"; break;
          case FPE_FLTOVF: reason = "floating point overflow"; break;
          case FPE_FLTUND: reason = "floating point underflow"; break;
          case FPE_FLTRES: reason = "floating point inexact result"; break;
          case FPE_FLTINV: reason = "invalid floating point operation"; break;
          case FPE_FLTSUB: reason = "subscript out of range"; break;
        }
        break;
      case SIGSEGV:
        switch (si->si_code) {
          case SEGV_MAPERR: reason = "address not mapped to object"; break;
          case SEGV_ACCERR: reason = "invalid permissions for mapped object";
                            break;
        }
        break;
#ifdef SIGBUS
      case SIGBUS:
        switch (si->si_code) {
          case BUS_ADRALN: reason = "invalid address alignment"; break;
          case BUS_ADRERR: reason = "non-existent physical address"; break;
        }
        break;
#endif
    }
  }

  if (si) {
    dprintf(STDERR_FILENO,
        "Terminating due to signal %d %s "
        "generated by pid=%d uid=%d. %s (%p)\n",
        signo, w_strsignal(signo), si->si_pid, si->si_uid,
        reason, si->si_value.sival_ptr);
  } else {
    dprintf(STDERR_FILENO, "Terminating due to signal %d %s. %s\n",
      signo, w_strsignal(signo), reason);
  }

#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS_FD)
  {
    void *array[24];
    size_t size = backtrace(array, sizeof(array)/sizeof(array[0]));
    backtrace_symbols_fd(array, size, STDERR_FILENO);
  }
#endif
  if (signo == SIGTERM) {
    w_request_shutdown();
    return;
  }
  abort();
}
#endif

#ifdef _WIN32
static LONG WINAPI exception_filter(LPEXCEPTION_POINTERS excep) {
  watchman::log(watchman::FATAL, "Unhandled win32 exception\n");
  return EXCEPTION_CONTINUE_SEARCH; // Terminate the process
}
#endif

void w_setup_signal_handlers(void) {
#ifndef _WIN32
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO|SA_RESETHAND;

  sigaction(SIGSEGV, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
#else
  // Don't show error dialogs for background service failures
  SetErrorMode(SEM_FAILCRITICALERRORS);
  // bridge OS exceptions into our FATAL logger so that we can
  // capture a stack trace.
  SetUnhandledExceptionFilter(exception_filter);
#endif

  std::set_terminate(
      []() { watchman::log(watchman::FATAL, "std::terminate was called\n"); });
}

static w_ctor_fn_type(register_thread_name) {
  pthread_key_create(&thread_name_key, free);
}
w_ctor_fn_reg(register_thread_name);

const char *w_set_thread_name(const char *fmt, ...) {
  char *name = NULL;
  va_list ap;
  free(pthread_getspecific(thread_name_key));
  va_start(ap, fmt);
  ignore_result(vasprintf(&name, fmt, ap));
  va_end(ap);
  pthread_setspecific(thread_name_key, name);
  return name;
}

void w_log(int level, WATCHMAN_FMT_STRING(const char *fmt), ...)
{
  va_list ap;
  va_start(ap, fmt);
  watchman::getLog().logVPrintf(
      static_cast<enum watchman::LogLevel>(level), fmt, ap);
  va_end(ap);
}

/* vim:ts=2:sw=2:et:
 */
