/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/Synchronized.h>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "watchman/Clock.h"
#include "watchman/Logging.h"
#include "watchman/PDU.h"
#include "watchman/PerfSample.h"
#include "watchman/watchman_root.h"
#include "watchman/watchman_stream.h"

struct watchman_client_subscription;

namespace watchman {

struct Query;
struct QueryResult;

} // namespace watchman

struct watchman_client : public std::enable_shared_from_this<watchman_client> {
  const uint64_t unique_id;
  std::unique_ptr<watchman_stream> stm;
  std::unique_ptr<watchman_event> ping;
  w_jbuffer_t reader, writer;
  bool client_mode{false};
  bool client_is_owner{false};
  enum w_pdu_type pdu_type;
  uint32_t capabilities;

  // The command currently being processed by dispatch_command
  json_ref current_command;
  watchman::PerfSample* perf_sample{nullptr};

  // Queue of things to send to the client.
  std::deque<json_ref> responses;

  // Logging Subscriptions
  std::shared_ptr<watchman::Publisher::Subscriber> debugSub;
  std::shared_ptr<watchman::Publisher::Subscriber> errorSub;

  watchman_client();
  explicit watchman_client(std::unique_ptr<watchman_stream>&& stm);
  virtual ~watchman_client();

  void enqueueResponse(json_ref&& resp, bool ping = true);
};

struct watchman_user_client;

enum class OnStateTransition { QueryAnyway, DontAdvance };

struct watchman_client_subscription
    : public std::enable_shared_from_this<watchman_client_subscription> {
  using Query = watchman::Query;

  struct LoggedResponse {
    // TODO: also track the time when the response was enqueued
    std::chrono::system_clock::time_point written;
    json_ref response;
  };

  std::shared_ptr<watchman::Root> root;
  w_string name;
  /* whether this subscription is paused */
  bool debug_paused = false;

  std::shared_ptr<Query> query;
  bool vcs_defer;
  uint32_t last_sub_tick{0};
  // map of statename => bool.  If true, policy is drop, else defer
  std::unordered_map<w_string, bool> drop_or_defer;
  std::weak_ptr<watchman_client> weakClient;

  std::deque<LoggedResponse> lastResponses;

  explicit watchman_client_subscription(
      const std::shared_ptr<watchman::Root>& root,
      std::weak_ptr<watchman_client> client);
  ~watchman_client_subscription();
  void processSubscription();

  std::shared_ptr<watchman_user_client> lockClient();
  json_ref buildSubscriptionResults(
      const std::shared_ptr<watchman::Root>& root,
      ClockSpec& position,
      OnStateTransition onStateTransition);

 private:
  using QueryResult = watchman::QueryResult;

  ClockSpec runSubscriptionRules(
      watchman_user_client* client,
      const std::shared_ptr<watchman::Root>& root);
  void updateSubscriptionTicks(QueryResult* res);
  void processSubscriptionImpl();
};

// Represents the server side session maintained for a client of
// the watchman per-user process
struct watchman_user_client : public watchman_client {
  /* map of subscription name => struct watchman_client_subscription */
  std::unordered_map<w_string, std::shared_ptr<watchman_client_subscription>>
      subscriptions;

  /* map of state-name => ClientStateAssertion
   * The values are owned by root::assertedStates */
  std::unordered_map<w_string, std::weak_ptr<watchman::ClientStateAssertion>>
      states;

  // Subscriber to root::unilateralResponses
  std::unordered_map<
      std::shared_ptr<watchman_client_subscription>,
      std::shared_ptr<watchman::Publisher::Subscriber>>
      unilateralSub;

  explicit watchman_user_client(std::unique_ptr<watchman_stream>&& stm);
  ~watchman_user_client() override;

  bool unsubByName(const w_string& name);
};

extern folly::Synchronized<std::unordered_set<std::shared_ptr<watchman_client>>>
    clients;

void w_client_vacate_states(struct watchman_user_client* client);
