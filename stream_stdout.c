/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

static int stdout_close(w_stm_t stm) {
  unused_parameter(stm);
  return -1;
}

static int stdout_read(w_stm_t stm, void *buf, int size) {
  unused_parameter(stm);
  unused_parameter(buf);
  unused_parameter(size);
  errno = EBADF;
  return -1;
}

static int stdout_write(w_stm_t stm, const void *buf, int size) {
  unused_parameter(stm);
  return write(STDOUT_FILENO, buf, size);
}

static void stdout_get_events(w_stm_t stm, w_evt_t *readable) {
  unused_parameter(stm);
  unused_parameter(readable);
  w_log(W_LOG_FATAL, "calling get_events on a stdout stm\n");
}

static void stdout_set_nonb(w_stm_t stm, bool nonb) {
  unused_parameter(stm);
  unused_parameter(nonb);
}

static bool stdout_rewind(w_stm_t stm) {
  unused_parameter(stm);
  return false;
}

static bool stdout_shutdown(w_stm_t stm) {
  unused_parameter(stm);
  return false;
}

static struct watchman_stream_ops stdout_ops = {
  stdout_close,
  stdout_read,
  stdout_write,
  stdout_get_events,
  stdout_set_nonb,
  stdout_rewind,
  stdout_shutdown
};

static struct watchman_stream stm_stdout = {
  (void*)&stdout_ops,
  &stdout_ops
};

w_stm_t w_stm_stdout(void) {
  return &stm_stdout;
}
