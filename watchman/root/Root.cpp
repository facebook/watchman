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

RootRecrawlInfo RootRecrawlInfo::fromJson(const json_ref& args) {
  RootRecrawlInfo result;
  json::assign(result.count, args, "count");
  json::assign(result.should_recrawl, args, "should-recrawl");
  json::assign(result.warning, args, "warning");
  return result;
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

RootQueryInfo RootQueryInfo::fromJson(const json_ref& args) {
  RootQueryInfo result;
  json::assign(result.elapsed_milliseconds, args, "elapsed-milliseconds");
  json::assign(
      result.cookie_sync_duration_milliseconds,
      args,
      "cookie-sync-duration-milliseconds");
  json::assign(
      result.generation_duration_milliseconds,
      args,
      "generation-duration-milliseconds");
  json::assign(
      result.render_duration_milliseconds,
      args,
      "render-duration-milliseconds");
  json::assign(
      result.view_lock_wait_duration_milliseconds,
      args,
      "view-lock-wait-duration-milliseconds");
  json::assign(result.state, args, "state");
  json::assign(result.client_pid, args, "client-pid");
  json::assign(result.request_id, args, "request-id");
  json::assign(result.query, args, "query");
  json::assign_if(result.subscription_name, args, "subscription-name");
  return result;
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

RootDebugStatus RootDebugStatus::fromJson(const json_ref& args) {
  RootDebugStatus result;
  json::assign(result.path, args, "path");
  json::assign(result.fstype, args, "fstype");
  json::assign(result.case_sensitive, args, "case_sensitive");
  json::assign(result.cookie_prefix, args, "cookie_prefix");
  json::assign(result.cookie_dir, args, "cookie_dir");
  json::assign(result.cookie_list, args, "cookie_list");
  json::assign(result.recrawl_info, args, "recrawl_info");
  json::assign(result.queries, args, "queries");
  json::assign(result.done_initial, args, "done_initial");
  json::assign(result.cancelled, args, "cancelled");
  json::assign(result.crawl_status, args, "crawl-status");
  return result;
}

} // namespace watchman
