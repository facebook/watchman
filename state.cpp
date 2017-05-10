/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_synchronized.h"

using watchman::FileDescriptor;

/** The state saving thread is responsible for writing out the
 * persistent information about the users watches.
 * It runs in its own thread so that we avoid the possibility
 * of self deadlock if various threads were to immediately
 * save the state when things are changing.
 *
 * This uses a simple condition variable to wait for and be
 * notified of state changes.
 */

namespace {
struct state {
  bool needsSave{false};
};
watchman::Synchronized<state, std::mutex> saveState;
std::condition_variable stateCond;
std::thread state_saver_thread;
}

static bool do_state_save(void);

static void state_saver() {
  bool do_save;

  w_set_thread_name("statesaver");

  while (!w_is_stopping()) {
    {
      auto state = saveState.wlock();
      if (!state->needsSave) {
        stateCond.wait(state.getUniqueLock());
      }
      do_save = state->needsSave;
      state->needsSave = false;
    }

    if (do_save) {
      do_state_save();
    }
  }
}

void w_state_shutdown(void) {
  if (dont_save_state) {
    return;
  }

  stateCond.notify_one();
  state_saver_thread.join();
}

bool w_state_load(void)
{
  json_error_t err;

  if (dont_save_state) {
    return true;
  }

  state_saver_thread = std::thread(state_saver);

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

std::unique_ptr<watchman_stream> w_mkstemp(char* templ) {
#if defined(_WIN32)
  char *name = _mktemp(templ);
  if (!name) {
    return nullptr;
  }
  // Most annoying aspect of windows is the latency around
  // file handle exclusivity.  We could avoid this dumb loop
  // by implementing our own mkostemp, but this is the most
  // expedient option for the moment.
  for (size_t attempts = 0; attempts < 10; ++attempts) {
    auto stm = w_stm_open(name, O_RDWR | O_CLOEXEC | O_CREAT | O_TRUNC, 0600);
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
  FileDescriptor fd;
# ifdef HAVE_MKOSTEMP
  fd = FileDescriptor(mkostemp(templ, O_CLOEXEC));
# else
  fd = FileDescriptor(mkstemp(templ));
# endif
  if (!fd) {
    return nullptr;
  }
  fd.setCloExec();

  return w_stm_fdopen(std::move(fd));
#endif
}

static bool do_state_save(void) {
  w_jbuffer_t buffer;

  auto state = json_object();

  auto file =
      w_stm_open(watchman_state_file, O_WRONLY | O_TRUNC | O_CREAT, 0600);
  if (!file) {
    w_log(
        W_LOG_ERR,
        "save_state: unable to open %s for write: %s\n",
        watchman_state_file,
        strerror(errno));
    return false;
  }

  state.set("version", typed_string_to_json(PACKAGE_VERSION, W_STRING_UNICODE));

  /* now ask the different subsystems to fill out the state */
  if (!w_root_save_state(state)) {
    return false;
  }

  /* we've prepared what we're going to save, so write it out */
  buffer.jsonEncodeToStream(state, file.get(), JSON_INDENT(4));
  return true;
}

/** Arranges for the state to be saved.
 * Does not immediately save the state. */
void w_state_save(void) {
  if (dont_save_state) {
    return;
  }

  saveState.wlock()->needsSave = true;
  stateCond.notify_one();
}

/* vim:ts=2:sw=2:et:
 */
