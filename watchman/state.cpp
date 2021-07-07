/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/String.h>
#include <folly/Synchronized.h>
#include "watchman/Errors.h"
#include "watchman/Logging.h"
#include "watchman/watchman.h"

using namespace watchman;

std::string watchman_state_file;
int dont_save_state = 0;

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
folly::Synchronized<state, std::mutex> saveState;
std::condition_variable stateCond;
std::thread state_saver_thread;
} // namespace

static bool do_state_save();

static void state_saver() noexcept {
  bool do_save;

  w_set_thread_name("statesaver");

  while (!w_is_stopping()) {
    {
      auto state = saveState.lock();
      if (!state->needsSave) {
        stateCond.wait(state.as_lock());
      }
      do_save = state->needsSave;
      state->needsSave = false;
    }

    if (do_save) {
      do_state_save();
    }
  }
}

void w_state_shutdown() {
  if (dont_save_state) {
    return;
  }

  stateCond.notify_one();
  state_saver_thread.join();
}

bool w_state_load() {
  if (dont_save_state) {
    return true;
  }

  state_saver_thread = std::thread(state_saver);

  json_ref state;
  try {
    state = json_load_file(watchman_state_file.c_str(), 0);
  } catch (const std::system_error& exc) {
    if (exc.code() == watchman::error_code::no_such_file_or_directory) {
      // No need to alarm anyone if we've never written a state file
      return false;
    }
    logf(
        ERR,
        "failed to load json from {}: {}\n",
        watchman_state_file,
        folly::exceptionStr(exc).toStdString());
    return false;
  } catch (const std::exception& exc) {
    logf(
        ERR,
        "failed to parse json from {}: {}\n",
        watchman_state_file,
        folly::exceptionStr(exc).toStdString());
    return false;
  }

  if (!w_root_load_state(state)) {
    return false;
  }

  return true;
}

#if defined(HAVE_MKOSTEMP) && defined(sun)
// Not guaranteed to be defined in stdlib.h
extern int mkostemp(char*, int);
#endif

std::unique_ptr<watchman_stream> w_mkstemp(char* templ) {
#if defined(_WIN32)
  char* name = _mktemp(templ);
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
      /* sleep override */ std::this_thread::sleep_for(
          std::chrono::microseconds(2000));
      continue;
    }
    return nullptr;
  }
  return nullptr;
#else
  FileDescriptor fd;
#ifdef HAVE_MKOSTEMP
  fd = FileDescriptor(
      mkostemp(templ, O_CLOEXEC), FileDescriptor::FDType::Generic);
#else
  fd = FileDescriptor(mkstemp(templ), FileDescriptor::FDType::Generic);
#endif
  if (!fd) {
    return nullptr;
  }
  fd.setCloExec();

  return w_stm_fdopen(std::move(fd));
#endif
}

static bool do_state_save() {
  w_jbuffer_t buffer;

  auto state = json_object();

  auto file = w_stm_open(
      watchman_state_file.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0600);
  if (!file) {
    log(ERR,
        "save_state: unable to open ",
        watchman_state_file,
        " for write: ",
        folly::errnoStr(errno),
        "\n");
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
void w_state_save() {
  if (dont_save_state) {
    return;
  }

  saveState.lock()->needsSave = true;
  stateCond.notify_one();
}

/* vim:ts=2:sw=2:et:
 */
