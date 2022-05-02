/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/query/QueryResult.h"

namespace watchman {

json_ref RenderResult::toJson() && {
  auto arr = json_array(std::move(results));
  if (templ) {
    json_array_set_template_new(arr, std::move(*templ));
  }
  return arr;
}

json_ref QueryDebugInfo::render() const {
  auto arr = json_array();
  for (auto& fn : cookieFileNames) {
    json_array_append(arr, w_string_to_json(fn));
  }
  return json_object({
      {"cookie_files", arr},
  });
}

} // namespace watchman
