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

#if defined(HAVE_MKOSTEMP) && defined(sun)
// Not guaranteed to be defined in stdlib.h
extern int mkostemp(char *, int);
#endif

w_stm_t w_mkstemp(char *templ)
{
#if defined(_WIN32)
  char *name = _mktemp(templ);
  if (!name) {
    return NULL;
  }
  return w_stm_open(name, O_RDWR|O_CLOEXEC|O_CREAT|O_TRUNC);
#else
  w_stm_t file;
  int fd;
# ifdef HAVE_MKOSTEMP
  fd = mkostemp(templ, O_CLOEXEC);
# else
  fd = mkstemp(templ);
# endif
  if (fd != -1) {
    w_set_cloexec(fd);
  }

  file = w_stm_fdopen(fd);
  if (!file) {
    close(fd);
  }
  return file;
#endif
}

bool w_state_save(void)
{
  json_t *state;
  w_jbuffer_t buffer;
  w_stm_t file = NULL;
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

  file = w_stm_open(watchman_state_file, O_WRONLY|O_TRUNC|O_CREAT, 0600);
  if (!file) {
    w_log(W_LOG_ERR, "save_state: unable to open %s for write: %s\n",
        watchman_state_file,
        strerror(errno));
    goto out;
  }

  json_object_set_new(state, "version", json_string(PACKAGE_VERSION));

  /* now ask the different subsystems to fill out the state */
  if (!w_root_save_state(state)) {
    goto out;
  }

  /* we've prepared what we're going to save, so write it out */
  w_json_buffer_write(&buffer, file, state, JSON_INDENT(4));
  w_stm_close(file);
  file = NULL;
  result = true;

out:
  if (file) {
    w_stm_close(file);
  }
  if (state) {
    json_decref(state);
  }
  w_json_buffer_free(&buffer);

  pthread_mutex_unlock(&state_lock);

  return result;
}

/* vim:ts=2:sw=2:et:
 */
