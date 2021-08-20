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

#pragma once
#include <utility>
namespace watchman {

// Remove key from the map. Returns true if any keys were removed.
template <typename Map, typename Key>
bool mapRemove(Map& map, Key& key) {
  return map.erase(key) > 0;
}

// Inserts Key->Value mapping if Key is not already present.
// Returns a boolean indicating whether insertion happened.
template <typename Map, typename Key, typename Value>
bool mapInsert(Map& map, const Key& key, const Value& value) {
  auto pair = map.insert(std::make_pair(key, value));
  return pair.second;
}

// Returns true if the map contains any of the passed keys
template <typename Map, typename Key>
bool mapContainsAny(const Map& map, const Key& key) {
  return map.find(key) != map.end();
}

// Returns true if the map contains any of the passed keys
template <typename Map, typename Key, typename... Args>
bool mapContainsAny(const Map& map, const Key& firstKey, Args... args) {
  return mapContainsAny(map, firstKey) || mapContainsAny(map, args...);
}

// Returns true if the map contains any of a list of passed keys
template <typename Map, typename Iterator>
bool mapContainsAnyOf(const Map& map, Iterator first, Iterator last) {
  for (auto it = first; it != last; ++it) {
    if (map.find(*it) != map.end()) {
      return true;
    }
  }
  return false;
}

// Returns Map[Key] or if it isn't present, returns a default value.
// if the default isn't specified, returns a default-constructed value.
template <class Map, typename Key = typename Map::key_type>
typename Map::mapped_type mapGetDefault(
    const Map& map,
    const Key& key,
    const typename Map::mapped_type& dflt = typename Map::mapped_type()) {
  auto pos = map.find(key);
  return (pos != map.end() ? pos->second : dflt);
}

} // namespace watchman
