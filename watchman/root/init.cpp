/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/String.h>
#include "watchman/Logging.h"
#include "watchman/QueryableView.h"
#include "watchman/TriggerCommand.h"
#include "watchman/root/Root.h"
#include "watchman/root/watchlist.h"
#include "watchman/watchman_opendir.h"

namespace watchman {

namespace {
/// Idle out watches that haven't had activity in several days
inline constexpr json_int_t kDefaultReapAge = 86400 * 5;
inline constexpr json_int_t kDefaultSettlePeriod = 20;
} // namespace

void ClientStateAssertions::queueAssertion(
    std::shared_ptr<ClientStateAssertion> assertion) {
  // Check to see if someone else has or had a pending claim for this
  // state and reject the attempt in that case
  auto state_q = states_.find(assertion->name);
  if (state_q != states_.end() && !state_q->second.empty()) {
    auto disp = state_q->second.back()->disposition;
    if (disp == ClientStateDisposition::PendingEnter ||
        disp == ClientStateDisposition::Asserted) {
      throw std::runtime_error(folly::to<std::string>(
          "state ",
          assertion->name.view(),
          " is already Asserted or PendingEnter"));
    }
  }
  states_[assertion->name].push_back(assertion);
}

json_ref ClientStateAssertions::debugStates() const {
  auto states = json_array();
  for (const auto& state_q : states_) {
    for (const auto& state : state_q.second) {
      auto obj = json_object();
      obj.set("name", w_string_to_json(state->name));
      switch (state->disposition) {
        case ClientStateDisposition::PendingEnter:
          obj.set("state", w_string_to_json("PendingEnter"));
          break;
        case ClientStateDisposition::Asserted:
          obj.set("state", w_string_to_json("Asserted"));
          break;
        case ClientStateDisposition::PendingLeave:
          obj.set("state", w_string_to_json("PendingLeave"));
          break;
        case ClientStateDisposition::Done:
          obj.set("state", w_string_to_json("Done"));
          break;
      }
      json_array_append(states, obj);
    }
  }
  return states;
}

bool ClientStateAssertions::removeAssertion(
    const std::shared_ptr<ClientStateAssertion>& assertion) {
  auto it = states_.find(assertion->name);
  if (it == states_.end()) {
    return false;
  }

  auto& queue = it->second;
  for (auto assertionIter = queue.begin(); assertionIter != queue.end();
       ++assertionIter) {
    if (*assertionIter == assertion) {
      assertion->disposition = ClientStateDisposition::Done;
      queue.erase(assertionIter);

      // If there are no more entries queued with this name, remove
      // the name from the states map.
      if (queue.empty()) {
        states_.erase(it);
      } else {
        // Now check to see who is at the front of the queue.  If
        // they are set to asserted and have a payload assigned, they
        // are a state-enter that is pending broadcast of the assertion.
        // We couldn't send it earlier without risking out of order
        // delivery wrt. vacating states.
        auto front = queue.front();
        if (front->disposition == ClientStateDisposition::Asserted &&
            front->enterPayload) {
          front->root->unilateralResponses->enqueue(
              std::move(front->enterPayload));
          front->enterPayload = nullptr;
        }
      }
      return true;
    }
  }

  return false;
}

bool ClientStateAssertions::isFront(
    const std::shared_ptr<ClientStateAssertion>& assertion) const {
  auto it = states_.find(assertion->name);
  if (it == states_.end()) {
    return false;
  }
  auto& queue = it->second;
  if (queue.empty()) {
    return false;
  }
  return queue.front() == assertion;
}

bool ClientStateAssertions::isStateAsserted(w_string stateName) const {
  auto it = states_.find(stateName);
  if (it == states_.end()) {
    return false;
  }
  auto& queue = it->second;
  for (auto& state : queue) {
    if (state->disposition == Asserted) {
      return true;
    }
  }
  return false;
}

void Root::applyIgnoreConfiguration() {
  auto ignores = config.get("ignore_dirs");
  if (!ignores) {
    return;
  }
  if (!ignores.isArray()) {
    logf(ERR, "ignore_dirs must be an array of strings\n");
    return;
  }

  for (size_t i = 0; i < json_array_size(ignores); i++) {
    auto jignore = json_array_get(ignores, i);

    if (!jignore.isString()) {
      logf(ERR, "ignore_dirs must be an array of strings\n");
      continue;
    }

    auto name = json_to_w_string(jignore);
    auto fullname = w_string::pathCat({root_path, name});
    ignore.add(fullname, false);
    logf(DBG, "ignoring {} recursively\n", fullname);
  }
}

Root::Root(
    const w_string& root_path,
    const w_string& fs_type,
    json_ref config_file,
    Configuration config_,
    std::shared_ptr<QueryableView> view,
    SaveGlobalStateHook saveGlobalStateHook)
    : RootConfig{root_path, fs_type, watchman::getCaseSensitivityForPath(root_path.c_str())},
      cookies(root_path),
      config_file(std::move(config_file)),
      config(std::move(config_)),
      trigger_settle(int(config.getInt("settle", kDefaultSettlePeriod))),
      gc_interval(
          int(config.getInt("gc_interval_seconds", DEFAULT_GC_INTERVAL))),
      gc_age(int(config.getInt("gc_age_seconds", DEFAULT_GC_AGE))),
      idle_reap_age(
          int(config.getInt("idle_reap_age_seconds", kDefaultReapAge))),
      unilateralResponses(std::make_shared<watchman::Publisher>()),
      saveGlobalStateHook_{std::move(saveGlobalStateHook)} {
  ++live_roots;
  applyIgnoreConfiguration();
  applyIgnoreVCSConfiguration();

  // This just opens and releases the dir.  If an exception is thrown
  // it will bubble up.
  w_dir_open(root_path.c_str());
  inner.view_ = std::move(view);

  inner.last_cmd_timestamp = std::chrono::steady_clock::now();

  if (!inner.view_->requiresCrawl) {
    // This watcher can resolve queries without needing a crawl.
    inner.done_initial = true;
    auto crawlInfo = recrawlInfo.wlock();
    crawlInfo->shouldRecrawl = false;
    crawlInfo->crawlStart = std::chrono::steady_clock::now();
    crawlInfo->crawlFinish = crawlInfo->crawlStart;
  }
}

Root::~Root() {
  logf(DBG, "root: final ref on {}\n", root_path);
  --live_roots;
}

void Root::addPerfSampleMetadata(PerfSample& sample) const {
  // Note: if the root lock isn't held, we may read inaccurate numbers for
  // some of these properties.  We're ok with that, and don't want to force
  // the root lock to be re-acquired just for this.
  auto meta = json_object(
      {{"path", w_string_to_json(root_path)},
       {"recrawl_count", json_integer(recrawlInfo.rlock()->recrawlCount)},
       {"case_sensitive",
        json_boolean(case_sensitive == CaseSensitivity::CaseSensitive)}});

  // During recrawl, the view may be re-assigned.  Protect against
  // reading a nullptr.
  auto view = this->view();
  if (view) {
    meta.set({{"watcher", w_string_to_json(view->getName())}});
  }

  sample.add_meta("root", std::move(meta));
}

} // namespace watchman
