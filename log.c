/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static void log_stack_trace(void)
{
#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  void *array[24];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, sizeof(array)/sizeof(array[0]));
  strings = backtrace_symbols(array, size);
  w_log(W_LOG_ERR, "Fatal error detected at:\n");

  for (i = 0; i < size; i++) {
    w_log(W_LOG_ERR, "%s\n", strings[i]);
  }

  free(strings);
#endif
}

int log_level = W_LOG_ERR;
static pthread_key_t thread_name_key;

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
  w_log(W_LOG_FATAL, "Terminating due to signal %d %s. %s (%p)\n",
      signo, strsignal(signo), reason, si ? si->si_value.sival_ptr : NULL);
}

void w_setup_signal_handlers(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;

  sigaction(SIGSEGV, &sa, NULL);
#ifdef SIGBUS
  sigaction(SIGBUS, &sa, NULL);
#endif
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
}


static __attribute__((constructor))
void register_thread_name(void) {
  pthread_key_create(&thread_name_key, free);
}

const char *w_get_thread_name(void) {
  const char *name = pthread_getspecific(thread_name_key);
  if (name) {
    return name;
  }
  return w_set_thread_name("%" PRIu32, (uint32_t)(uintptr_t)pthread_self());
}

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

void w_log(int level, const char *fmt, ...)
{
  char buf[4096];
  va_list ap;
  int len;
  bool fatal = false;
  struct timeval tv;
  char timebuf[64];
  struct tm tm;

  bool should_log_to_stderr = level <= log_level;
  bool should_log_to_clients = w_should_log_to_clients(level);

  if (!(should_log_to_stderr || should_log_to_clients)) {
    // Don't bother formatting the log message if nobody's listening.
    return;
  }

  if (level == W_LOG_FATAL) {
    level = W_LOG_ERR;
    fatal = true;
  }

  gettimeofday(&tv, NULL);
  localtime_r(&tv.tv_sec, &tm);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);

  len = snprintf(buf, sizeof(buf), "%s,%03d: [%s] ",
         timebuf, (int)tv.tv_usec / 1000, w_get_thread_name());
  va_start(ap, fmt);
  vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);

  len = strlen(buf);
  if (buf[len - 1] != '\n') {
    if (len < (int)sizeof(buf) - 1) {
      buf[len] = '\n';
      buf[len + 1] = 0;
      len++;
    } else {
      buf[len - 1] = '\n';
    }
  }

  if (should_log_to_stderr) {
    ignore_result(write(STDERR_FILENO, buf, len));
  }

  if (should_log_to_clients) {
    w_log_to_clients(level, buf);
  }

  if (fatal) {
    log_stack_trace();
    abort();
  }
}

/* vim:ts=2:sw=2:et:
 */
