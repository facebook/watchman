/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

std::unique_ptr<watchman_stream> w_stm_connect(
    const char* path,
    int timeoutms) {
#ifdef _WIN32
  return w_stm_connect_named_pipe(path, timeoutms);
#else
  return w_stm_connect_unix(path, timeoutms);
#endif
}

int w_stm_read(w_stm_t stm, void *buf, int size) {
  if (!stm) {
    errno = EBADF;
    return -1;
  }
  return stm->read(buf, size);
}

int w_stm_write(w_stm_t stm, const void *buf, int size) {
  if (!stm) {
    errno = EBADF;
    return -1;
  }
  return stm->write(buf, size);
}

void w_stm_get_events(w_stm_t stm, w_evt_t *readable) {
  if (!stm) {
    errno = EBADF;
    return;
  }
  *readable = stm->getEvents();
}

void w_stm_set_nonblock(w_stm_t stm, bool nonb) {
  if (!stm) {
    errno = EBADF;
    return;
  }
  stm->setNonBlock(nonb);
}

bool w_stm_rewind(w_stm_t stm) {
  if (!stm) {
    errno = EBADF;
    return false;
  }
  return stm->rewind();
}

bool w_stm_shutdown(w_stm_t stm) {
  if (!stm) {
    errno = EBADF;
    return false;
  }
  return stm->shutdown();
}

bool w_stm_peer_is_owner(w_stm_t stm) {
  if (!stm) {
    errno = EBADF;
    return false;
  }
  return stm->peerIsOwner();
}
