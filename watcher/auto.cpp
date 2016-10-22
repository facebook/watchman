/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

Watcher::Watcher(const char* name, unsigned flags) : name(name), flags(flags) {}

Watcher::~Watcher() {}

bool Watcher::startWatchFile(struct watchman_file*) {
  return true;
}

bool Watcher::start(w_root_t*) {
  return true;
}

void Watcher::signalThreads() {}

// These are listed in order of preference in the case that a
// given system offers multiple choices
static Watcher* available_watchers[] = {
#if HAVE_FSEVENTS
    fsevents_watcher,
#endif
#if defined(HAVE_PORT_CREATE)
    // We prefer portfs if you have both portfs and inotify on the assumption
    // that this is an Illumos based system with both and that the native
    // mechanism will yield more correct behavior.
    // https://github.com/facebook/watchman/issues/84
    portfs_watcher,
#endif
#if defined(HAVE_INOTIFY_INIT)
    inotify_watcher,
#endif
#if defined(HAVE_KQUEUE)
    kqueue_watcher,
#endif
#if defined(_WIN32)
    win32_watcher,
#endif
    nullptr};

bool w_watcher_init(w_root_t *root, char **errmsg) {
  const char* watcher_name = root->config.getString("watcher", "auto");
  Watcher* ops = nullptr;
  char* first_err = nullptr;
  int i;

  if (strcmp(watcher_name, "auto")) {
    // If they asked for a specific one, let's try to find it
    for (i = 0; available_watchers[i]; i++) {
      if (strcmp(available_watchers[i]->name, watcher_name)) {
        continue;
      }
      ops = available_watchers[i];

      if (ops->initNew(root, &first_err)) {
        goto done;
      }
    }
  }

  // The auto, or fallback behavior
  for (i = 0; available_watchers[i]; i++) {
    char* err = nullptr;

    if (ops == available_watchers[i]) {
      // Already tried this one above
      continue;
    }

    if (available_watchers[i]->initNew(root, &err)) {
      goto done;
    }

    if (first_err) {
      char* tmp = nullptr;

      ignore_result(asprintf(&tmp, "%s. %s", first_err, err));
      free(err);
      free(first_err);
      first_err = tmp;
    } else {
      first_err = err;
    }
  }

  if (!root->inner.watcher) {
    if (first_err) {
      *errmsg = first_err;
    } else {
      // Unpossible!
      ignore_result(asprintf(errmsg,
            "No watchers available on this system!?"));
    }
    return false;
  }

done:
  root->inner.view->watcher = root->inner.watcher;
  w_log(
      W_LOG_ERR,
      "root %s using watcher mechanism %s (%s was requested)\n",
      root->root_path.c_str(),
      root->inner.watcher->name,
      watcher_name);
  free(first_err);
  return true;
}

/* vim:ts=2:sw=2:et:
 */
