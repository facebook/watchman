/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"

// These are listed in order of preference in the case that a
// given system offers multiple choices
static struct watchman_ops *available_watchers[] = {
#if HAVE_FSEVENTS
  &fsevents_watcher,
#endif
#if defined(HAVE_PORT_CREATE)
  // We prefer portfs if you have both portfs and inotify on the assumption
  // that this is an Illumos based system with both and that the native
  // mechanism will yield more correct behavior.
  // https://github.com/facebook/watchman/issues/84
  &portfs_watcher,
#endif
#if defined(HAVE_INOTIFY_INIT)
  &inotify_watcher,
#endif
#if defined(HAVE_KQUEUE)
  &kqueue_watcher,
#endif
#if defined(_WIN32)
  &win32_watcher,
#endif
  NULL
};

bool w_watcher_init(w_root_t *root, char **errmsg) {
  const char *watcher_name = cfg_get_string(root, "watcher", "auto");
  struct watchman_ops *ops = NULL;
  char *first_err = NULL;
  int i;

  root->watcher_ops = NULL;

  if (strcmp(watcher_name, "auto")) {
    // If they asked for a specific one, let's try to find it
    for (i = 0; available_watchers[i]; i++) {
      if (strcmp(available_watchers[i]->name, watcher_name)) {
        continue;
      }
      ops = available_watchers[i];

      if (ops->root_init(root, &first_err)) {
        root->watcher_ops = ops;
        goto done;
      }
    }
  }

  // The auto, or fallback behavior
  for (i = 0; available_watchers[i]; i++) {
    char *err = NULL;

    if (ops == available_watchers[i]) {
      // Already tried this one above
      continue;
    }

    if (available_watchers[i]->root_init(root, &err)) {
      root->watcher_ops = available_watchers[i];
      goto done;
    }

    if (first_err) {
      char *tmp = NULL;

      ignore_result(asprintf(&tmp, "%s. %s", first_err, err));
      free(err);
      free(first_err);
      first_err = tmp;
    } else {
      first_err = err;
    }
  }

  if (!root->watcher_ops) {
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
  w_log(W_LOG_ERR, "root %.*s using watcher mechanism %s (%s was requested)\n",
      root->root_path->len, root->root_path->buf,
      root->watcher_ops->name,
      watcher_name);
  free(first_err);
  return true;
}

/* vim:ts=2:sw=2:et:
 */
