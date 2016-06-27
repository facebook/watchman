/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

w_stm_t w_stm_connect(const char *path, int timeoutms) {
#ifdef _WIN32
  return w_stm_connect_named_pipe(path, timeoutms);
#else
  return w_stm_connect_unix(path, timeoutms);
#endif
}

int w_stm_close(w_stm_t stm) {
  int res;
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return -1;
  }
  res = stm->ops->op_close(stm);
  if (res == 0) {
    stm->ops = NULL;
    stm->handle = NULL;
    free(stm);
  }
  return res;
}

int w_stm_read(w_stm_t stm, void *buf, int size) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return -1;
  }
  return stm->ops->op_read(stm, buf, size);
}

int w_stm_write(w_stm_t stm, const void *buf, int size) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return -1;
  }
  return stm->ops->op_write(stm, buf, size);
}

void w_stm_get_events(w_stm_t stm, w_evt_t *readable) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return;
  }
  stm->ops->op_get_events(stm, readable);
}

void w_stm_set_nonblock(w_stm_t stm, bool nonb) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return;
  }
  stm->ops->op_set_nonblock(stm, nonb);
}

bool w_stm_rewind(w_stm_t stm) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return false;
  }
  return stm->ops->op_rewind(stm);
}

bool w_stm_shutdown(w_stm_t stm) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return false;
  }
  return stm->ops->op_shutdown(stm);
}

bool w_stm_peer_is_owner(w_stm_t stm) {
  if (!stm || stm->handle == NULL || stm->ops == NULL) {
    errno = EBADF;
    return false;
  }
  if (!stm->ops->op_peer_is_owner) {
    return false;
  }
  return stm->ops->op_peer_is_owner(stm);
}
