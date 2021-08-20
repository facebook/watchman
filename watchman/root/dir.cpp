/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman_dir.h"
#include "watchman/watchman_file.h"

void watchman_dir::Deleter::operator()(watchman_file* file) const {
  free_file_node(file);
}

watchman_dir::watchman_dir(w_string name, watchman_dir* parent)
    : name(std::move(name)), parent(parent) {}

w_string watchman_dir::getFullPath() const {
  return getFullPathToChild(w_string_piece());
}

watchman_file* watchman_dir::getChildFile(w_string_piece name) const {
  auto it = files.find(name);
  if (it == files.end()) {
    return nullptr;
  }
  return it->second.get();
}

watchman_dir* watchman_dir::getChildDir(w_string_piece name) const {
  auto it = dirs.find(name);
  if (it == dirs.end()) {
    return nullptr;
  }
  return it->second.get();
}

w_string watchman_dir::getFullPathToChild(w_string_piece extra) const {
  uint32_t length = 0;
  w_string_t* s;
  char *buf, *end;

  if (extra.size()) {
    length = extra.size() + 1 /* separator */;
  }
  for (const watchman_dir* d = this; d; d = d->parent) {
    length += d->name.size() + 1 /* separator OR final NUL terminator */;
  }

  s = (w_string_t*)(new char[sizeof(*s) + length]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = length - 1;
  buf = const_cast<char*>(s->buf);
  end = buf + s->len;

  *end = 0;
  if (extra.size()) {
    end -= extra.size();
    memcpy(end, extra.data(), extra.size());
  }
  for (const watchman_dir* d = this; d; d = d->parent) {
    if (d != this || (extra.size())) {
      --end;
      *end = '/';
    }
    end -= d->name.size();
    memcpy(end, d->name.data(), d->name.size());
  }

  return w_string(s, false);
}

/* vim:ts=2:sw=2:et:
 */
