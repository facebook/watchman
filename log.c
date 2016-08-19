/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_LIBUNWIND_H
#include <libunwind.h>
#endif

#define MAX_BT_FRAMES 24

static char **get_backtrace(size_t *n_frames, void *frames[MAX_BT_FRAMES]) {
#if defined(HAVE_UNW_INIT_LOCAL) && defined(HAVE_LIBUNWIND_H)
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_word_t ip;
  char **strings = calloc(MAX_BT_FRAMES, sizeof(char*));
  size_t n = 0;

  *n_frames = 0;
  if (!strings) {
    return NULL;
  }

  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  while (unw_step(&cursor) > 0) {
    char name[128];
    unw_word_t off;

    unw_get_reg(&cursor, UNW_REG_IP, &ip);

    off = 0;
    if (unw_get_proc_name(&cursor, name, sizeof(name), &off) != 0) {
      strcpy(name, "???");
    }

    asprintf(&strings[n], "#%" PRIsize_t " %p %s`%lx", n, (void *)(intptr_t)ip,
             name, off);

    frames[n] = (void*)(intptr_t)ip;

    ++n;
  }

  *n_frames = n;
  return strings;

#elif defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  size_t size;

  size = backtrace(frames, MAX_BT_FRAMES);
  *n_frames = size;
  return backtrace_symbols(frames, size);
#else
  *n_frames = 0;
  return NULL;
#endif
}

#ifndef _WIN32
/** Attempt to run addr2line on the frames as a last-ditch effort
 * to get some line number information */
void addr2line_frames(void *frames[MAX_BT_FRAMES], size_t n_frames) {
  char exename[WATCHMAN_NAME_MAX];
  int result;
  size_t i, argc;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_t actions;
  pid_t pid;
  char addrs[MAX_BT_FRAMES][32];
  char *argv[64] = {
    "addr2line",
    "-e",
    exename,
    "--inlines",
    NULL,
  };

  // Figure out our executable path.  Most likely only successful on
  // Linux, but safe to try on other systems.
  result = readlink("/proc/self/exe", exename, sizeof(exename));
  if (result <= 0) {
    return;
  }
  exename[result] = '\0';

  // Compute the end of argv; we'll append addresses there
  for (argc = 0; argv[argc]; ++argc) {
    ;
  }

  // Generate hex address strings and append them to argv
  for (i = 0; i < n_frames; ++i) {
    if (frames[i] == 0) {
      break;
    }

    snprintf(addrs[i], sizeof(addrs[i]), " 0x%lx", (intptr_t)frames[i]);
    argv[argc++] = addrs[i];
    argv[argc] = NULL;
  }

  // And run it!
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
  dprintf(STDERR_FILENO, "Attempting to symbolize stack trace via addr2line:\n");
  posix_spawnp(&pid, argv[0], &actions, &attr, argv, NULL);

  // We can't waitpid reliably here because of our reaper thread :-/
}
#endif

static void log_stack_trace(void)
{
  char **strings;
  size_t i, n;
  void *frames[MAX_BT_FRAMES];

  strings = get_backtrace(&n, frames);
  w_log(W_LOG_ERR, "Fatal error detected at:\n");

  for (i = 0; i < n; i++) {
    w_log(W_LOG_ERR, "%s\n", strings[i]);
  }

  free(strings);
}

int log_level = W_LOG_ERR;
static pthread_key_t thread_name_key;

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

  {
    size_t i, n;
    void *frames[MAX_BT_FRAMES];
    char **strings = get_backtrace(&n, frames);

    for (i = 0; i < n; ++i) {
      dprintf(STDERR_FILENO, "%s\n", strings[i]);
    }

    addr2line_frames(frames, n);

    // Deliberately leaking strings just in case crash was due to
    // heap corruption.
  }

  if (signo == SIGTERM) {
    w_request_shutdown();
    return;
  }
  abort();
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
#endif
}

static w_ctor_fn_type(register_thread_name) {
  pthread_key_create(&thread_name_key, free);
}
w_ctor_fn_reg(register_thread_name);

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

void w_log(int level, WATCHMAN_FMT_STRING(const char *fmt), ...)
{
  char buf[4096*4];
  va_list ap;
  int len, len2;
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
#ifdef _WIN32
  tm = *localtime(&tv.tv_sec);
#else
  localtime_r(&tv.tv_sec, &tm);
#endif
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm);

  len = snprintf(buf, sizeof(buf), "%s,%03d: [%s] ",
         timebuf, (int)tv.tv_usec / 1000, w_get_thread_name());
  va_start(ap, fmt);
  len2 = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
  va_end(ap);

  if (len2 == -1) {
    // Truncated.  Ensure that we have a NUL terminator
    buf[sizeof(buf)-1] = 0;
  }

  len = (int)strlen(buf);

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
    ignore_result(write(STDERR_FILENO, buf, (unsigned int)len));
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
