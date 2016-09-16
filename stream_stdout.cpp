/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

static inline int which_fd(w_stm_t stm);

static int stdio_close(w_stm_t stm) {
  unused_parameter(stm);
  return -1;
}

static int stdio_read(w_stm_t stm, void *buf, int size) {
  return read(which_fd(stm), buf, size);
}

static int stdio_write(w_stm_t stm, const void *buf, int size) {
  return write(which_fd(stm), buf, size);
}

static void stdio_get_events(w_stm_t stm, w_evt_t *readable) {
  unused_parameter(stm);
  unused_parameter(readable);
  w_log(W_LOG_FATAL, "calling get_events on a stdio stm\n");
}

static void stdio_set_nonb(w_stm_t stm, bool nonb) {
  unused_parameter(stm);
  unused_parameter(nonb);
}

static bool stdio_rewind(w_stm_t stm) {
  unused_parameter(stm);
  return false;
}

static bool stdio_shutdown(w_stm_t stm) {
  unused_parameter(stm);
  return false;
}

static struct watchman_stream_ops stdio_ops = {
  stdio_close,
  stdio_read,
  stdio_write,
  stdio_get_events,
  stdio_set_nonb,
  stdio_rewind,
  stdio_shutdown,
  NULL
};

static struct watchman_stream stm_stdout = {
  (void*)&stdio_ops,
  &stdio_ops
};

static struct watchman_stream stm_stdin = {
  (void*)&stdio_ops,
  &stdio_ops
};

static inline int which_fd(w_stm_t stm) {
  if (stm == &stm_stdout) {
    return STDOUT_FILENO;
  }
  return STDIN_FILENO;
}

w_stm_t w_stm_stdout(void) {
  return &stm_stdout;
}

w_stm_t w_stm_stdin(void) {
  return &stm_stdin;
}
