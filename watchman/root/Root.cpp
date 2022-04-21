/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/root/Root.h"

namespace watchman {

json_ref RootRecrawlInfo::toJson() const {
  return json_object({
      {"count", json::to(count)},
      {"should-recrawl", json::to(should_recrawl)},
      {"warning", json::to(warning)},
  });
}

json_ref RootQueryInfo::toJson() const {
  auto obj = json_object({
      {"elapsed-milliseconds", json::to(elapsed_milliseconds)},
      {"cookie-sync-duration-milliseconds",
       json::to(cookie_sync_duration_milliseconds)},
      {"generation-duration-milliseconds",
       json::to(generation_duration_milliseconds)},
      {"render-duration-milliseconds", json::to(render_duration_milliseconds)},
      {"view-lock-wait-duration-milliseconds",
       json::to(view_lock_wait_duration_milliseconds)},
      {"state", json::to(state)},
      {"client-pid", json::to(client_pid)},
      {"request-id", json::to(request_id)},
      {"query", json::to(query)},
  });
  if (subscription_name) {
    obj.set("subscription-name", json::to(subscription_name.value()));
  }
  return obj;
}

json_ref RootDebugStatus::toJson() const {
  return json_object({
      {"path", json::to(path)},
      {"fstype", json::to(fstype)},
      {"case_sensitive", json::to(case_sensitive)},
      {"cookie_prefix", json::to(cookie_prefix)},
      {"cookie_dir", json::to(cookie_dir)},
      {"cookie_list", json::to(cookie_list)},
      {"recrawl_info", json::to(recrawl_info)},
      {"queries", json::to(queries)},
      {"done_initial", json::to(done_initial)},
      {"cancelled", json::to(cancelled)},
      {"crawl-status", json::to(crawl_status)},
  });
}

} // namespace watchman
