/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include <vector>

watchman::Synchronized<std::unordered_map<w_string, w_root_t*>> watched_roots;
std::atomic<long> live_roots{0};

bool remove_root_from_watched(
    w_root_t* root /* don't care about locked state */) {
  auto map = watched_roots.wlock();
  auto it = map->find(root->root_path);
  if (it == map->end()) {
    return false;
  }
  // it's possible that the root has already been removed and replaced with
  // another, so make sure we're removing the right object
  if (it->second == root) {
    map->erase(it);
    w_root_delref_raw(root);
    return true;
  }
  return false;
}

// Given a filename, walk the current set of watches.
// If a watch is a prefix match for filename then we consider it to
// be an enclosing watch and we'll return the root path and the relative
// path to filename.
// Returns NULL if there were no matches.
// If multiple watches have the same prefix, it is undefined which one will
// match.
char *w_find_enclosing_root(const char *filename, char **relpath) {
  w_root_t *root = NULL;
  w_string name(filename, W_STRING_BYTE);
  char *prefix = NULL;

  {
    auto map = watched_roots.rlock();
    for (const auto& it : *map) {
      auto root_name = it.first;
      if (w_string_startswith(name, root_name) &&
          (name.size() == root_name.size() /* exact match */ ||
           is_slash(
               name.data()[root_name.size()]) /* dir container matches */)) {
        root = it.second;
        w_root_addref(root);
        break;
      }
    }
  }

  if (!root) {
    goto out;
  }

  // extract the path portions
  prefix = (char*)malloc(root->root_path.size() + 1);
  if (!prefix) {
    goto out;
  }
  memcpy(prefix, filename, root->root_path.size());
  prefix[root->root_path.size()] = '\0';

  if (root->root_path.size() == name.size()) {
    *relpath = NULL;
  } else {
    *relpath = strdup(filename + root->root_path.size() + 1);
  }

out:
  if (root) {
    w_root_delref_raw(root);
  }

  return prefix;
}

json_ref w_root_stop_watch_all(void) {
  std::vector<w_root_t*> roots;
  json_ref stopped = json_array();

  // Funky looking loop because w_root_cancel() needs to acquire the
  // watched_roots wlock and will invalidate any iterators we might
  // otherwise have held.  Therefore we just loop until the map is
  // empty.
  while (true) {
    w_root_t* root;

    {
      auto map = watched_roots.wlock();
      if (map->empty()) {
        break;
      }

      auto it = map->begin();
      root = it->second;
      w_root_addref(root);
    }

    w_root_cancel(root);
    json_array_append_new(stopped, w_string_to_json(root->root_path));
    w_root_delref_raw(root);
  }

  w_state_save();

  return stopped;
}

json_ref w_root_watch_list_to_json(void) {
  auto arr = json_array();

  auto map = watched_roots.rlock();
  for (const auto& it : *map) {
    auto root = it.second;
    json_array_append_new(arr, w_string_to_json(root->root_path));
  }

  return arr;
}

bool w_root_save_state(json_ref& state) {
  bool result = true;

  auto watched_dirs = json_array();

  w_log(W_LOG_DBG, "saving state\n");

  {
    auto map = watched_roots.rlock();
    for (const auto& it : *map) {
      auto root = it.second;
      struct read_locked_watchman_root lock;
      struct unlocked_watchman_root unlocked = {root};

      auto obj = json_object();

      json_object_set_new(
          obj, "path", w_string_to_json(unlocked.root->root_path));

      w_root_read_lock(&unlocked, "w_root_save_state", &lock);
      auto triggers = w_root_trigger_list_to_json(&lock);
      w_root_read_unlock(&lock, &unlocked);
      json_object_set_new(obj, "triggers", std::move(triggers));

      json_array_append_new(watched_dirs, std::move(obj));
    }
  }

  json_object_set_new(state, "watched", std::move(watched_dirs));

  return result;
}

json_ref w_root_trigger_list_to_json(struct read_locked_watchman_root* lock) {
  auto arr = json_array();
  {
    auto map = lock->root->triggers.rlock();
    for (const auto& it : *map) {
      const auto& cmd = it.second;
      json_array_append(arr, cmd->definition);
    }
  }

  return arr;
}

bool w_root_load_state(const json_ref& state) {
  size_t i;

  auto watched = state.get_default("watched");
  if (!watched) {
    return true;
  }

  if (!json_is_array(watched)) {
    return false;
  }

  for (i = 0; i < json_array_size(watched); i++) {
    const auto& obj = watched.at(i);
    bool created = false;
    const char *filename;
    size_t j;
    char *errmsg = NULL;
    struct unlocked_watchman_root unlocked;

    auto triggers = obj.get_default("triggers");
    filename = json_string_value(json_object_get(obj, "path"));
    if (!root_resolve(filename, true, &created, &errmsg, &unlocked)) {
      free(errmsg);
      continue;
    }

    {
      auto wlock = unlocked.root->triggers.wlock();
      auto& map = *wlock;

      /* re-create the trigger configuration */
      for (j = 0; j < json_array_size(triggers); j++) {
        const auto& tobj = triggers.at(j);

        // Legacy rules format
        auto rarray = tobj.get_default("rules");
        if (rarray) {
          continue;
        }

        auto cmd = w_build_trigger_from_def(unlocked.root, tobj, &errmsg);
        if (!cmd) {
          w_log(
              W_LOG_ERR,
              "loading trigger for %s: %s\n",
              unlocked.root->root_path.c_str(),
              errmsg);
          free(errmsg);
          continue;
        }

        map[cmd->triggername] = std::move(cmd);
      }
    }

    if (created) {
      if (!root_start(unlocked.root, &errmsg)) {
        w_log(
            W_LOG_ERR,
            "root_start(%s) failed: %s\n",
            unlocked.root->root_path.c_str(),
            errmsg);
        free(errmsg);
        w_root_cancel(unlocked.root);
      }
    }

    w_root_delref(&unlocked);
  }

  return true;
}

void w_root_free_watched_roots(void) {
  int last, interval;
  time_t started;

  // Reap any children so that we can release their
  // references on the root
  w_reap_children(true);

  {
    auto map = watched_roots.rlock();
    for (const auto& it : *map) {
      auto root = it.second;
      if (!w_root_cancel(root)) {
        signal_root_threads(root);
      }
    }
  }

  last = live_roots;
  time(&started);
  w_log(W_LOG_DBG, "waiting for roots to cancel and go away %d\n", last);
  interval = 100;
  for (;;) {
    auto current = live_roots.load();
    if (current == 0) {
      break;
    }
    if (time(NULL) > started + 3) {
      w_log(W_LOG_ERR, "%ld roots were still live at exit\n", current);
      break;
    }
    if (current != last) {
      w_log(W_LOG_DBG, "waiting: %ld live\n", current);
      last = current;
    }
    usleep(interval);
    interval = MIN(interval * 2, 1000000);
  }

  w_log(W_LOG_DBG, "all roots are gone\n");
}

/* vim:ts=2:sw=2:et:
 */
