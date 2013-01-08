/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

bool w_state_load(void)
{
  json_t *state = NULL;
  bool result = false;
  json_error_t err;

  if (dont_save_state) {
    return true;
  }

  state = json_load_file(watchman_state_file, 0, &err);

  if (!state) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
        watchman_state_file,
        err.text);
    goto out;
  }

  if (!w_root_load_state(state)) {
    goto out;
  }

  result = true;

out:
  if (state) {
    json_decref(state);
  }

  return result;
}

bool w_state_save(void)
{
  json_t *state;
  w_jbuffer_t buffer;
  int fd = -1;
  char tmpname[WATCHMAN_NAME_MAX];
  bool result = false;

  if (dont_save_state) {
    return true;
  }

  pthread_mutex_lock(&state_lock);

  state = json_object();

  if (!w_json_buffer_init(&buffer)) {
    w_log(W_LOG_ERR, "save_state: failed to init json buffer\n");
    goto out;
  }

  snprintf(tmpname, sizeof(tmpname), "%sXXXXXX",
      watchman_state_file);
  fd = mkstemp(tmpname);
  if (fd == -1) {
    w_log(W_LOG_ERR, "save_state: unable to create temporary file: %s\n",
        strerror(errno));
    goto out;
  }

  json_object_set_new(state, "version", json_string(PACKAGE_VERSION));

  /* now ask the different subsystems to fill out the state */
  if (!w_root_save_state(state)) {
    goto out;
  }

  /* we've prepared what we're going to save, so write it out */
  w_json_buffer_write(&buffer, fd, state, JSON_INDENT(4));

  /* atomically replace the old contents */
  result = rename(tmpname, watchman_state_file) == 0;

out:
  if (state) {
    json_decref(state);
  }
  w_json_buffer_free(&buffer);
  if (fd != -1) {
    if (!result) {
      // If we didn't succeed, remove our temporary file
      unlink(tmpname);
    }
    close(fd);
  }

  pthread_mutex_unlock(&state_lock);

  return result;
}

/* vim:ts=2:sw=2:et:
 */

