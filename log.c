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
  vasprintf(&name, fmt, ap);
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

  if (level <= log_level) {
    ignore_result(write(STDERR_FILENO, buf, len));
  }

  w_log_to_clients(level, buf);

  if (fatal) {
    log_stack_trace();
    abort();
  }
}

/* vim:ts=2:sw=2:et:
 */
