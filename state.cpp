/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/** The state saving thread is responsible for writing out the
 * persistent information about the users watches.
 * It runs in its own thread so that we avoid the possibility
 * of self deadlock if various threads were to immediately
 * save the state when things are changing.
 *
 * This uses a simple condition variable to wait for and be
 * notified of state changes.
 */

static pthread_t state_saver_thread;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond;
static bool need_save = false;

static bool do_state_save(void);

static void *state_saver(void *unused) {
  bool do_save;
  unused_parameter(unused);

  w_set_thread_name("statesaver");

  while (!w_is_stopping()) {
    pthread_mutex_lock(&state_lock);
    if (!need_save) {
      pthread_cond_wait(&state_cond, &state_lock);
    }
    do_save = need_save;
    need_save = false;
    pthread_mutex_unlock(&state_lock);

    if (do_save) {
      do_state_save();
    }
  }
  return NULL;
}

void w_state_shutdown(void) {
  void *result;

  if (dont_save_state) {
    return;
  }

  pthread_cond_signal(&state_cond);
  pthread_join(state_saver_thread, &result);
}

bool w_state_load(void)
{
  json_error_t err;

  if (dont_save_state) {
    return true;
  }

  pthread_cond_init(&state_cond, NULL);
  errno = pthread_create(&state_saver_thread, NULL, state_saver, NULL);
  if (errno) {
    w_log(W_LOG_FATAL, "failed to spawn state thread: %s\n", strerror(errno));
  }

  auto state = json_load_file(watchman_state_file, 0, &err);

  if (!state) {
    w_log(W_LOG_ERR, "failed to parse json from %s: %s\n",
        watchman_state_file,
        err.text);
    return false;
  }

  if (!w_root_load_state(state)) {
    return false;
  }

  return true;
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
  // Most annoying aspect of windows is the latency around
  // file handle exclusivity.  We could avoid this dumb loop
  // by implementing our own mkostemp, but this is the most
  // expedient option for the moment.
  for (size_t attempts = 0; attempts < 10; ++attempts) {
    auto stm = w_stm_open(name, O_RDWR | O_CLOEXEC | O_CREAT | O_TRUNC);
    if (stm) {
      return stm;
    }
    if (errno == EACCES) {
      /* sleep override */ usleep(2000);
      continue;
    }
    return nullptr;
  }
  return nullptr;
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

static bool do_state_save(void)
{
  w_jbuffer_t buffer;
  w_stm_t file = NULL;
  bool result = false;

  auto state = json_object();

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

  state.set("version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE));

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
  w_json_buffer_free(&buffer);

  return result;
}

/** Arranges for the state to be saved.
 * Does not immediately save the state. */
void w_state_save(void) {
  if (dont_save_state) {
    return;
  }

  pthread_mutex_lock(&state_lock);
  need_save = true;
  pthread_mutex_unlock(&state_lock);

  pthread_cond_signal(&state_cond);
}

/* vim:ts=2:sw=2:et:
 */
