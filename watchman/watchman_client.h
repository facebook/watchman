/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <folly/Synchronized.h>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "watchman/Clock.h"
#include "watchman/Logging.h"
#include "watchman/watchman_perf.h"
#include "watchman_pdu.h"

struct w_query;
struct w_query_res;
struct watchman_client_subscription;

namespace watchman {

enum ClientStateDisposition {
  PendingEnter,
  Asserted,
  PendingLeave,
  Done,
};

class ClientStateAssertion {
 public:
  const std::shared_ptr<watchman_root> root; // Holds a ref on the root
  const w_string name;
  // locking: You must hold root->assertedStates lock to access this member
  ClientStateDisposition disposition{PendingEnter};

  // Deferred payload to send when this assertion makes it to the front
  // of the queue.
  // locking: You must hold root->assertedStates lock to access this member.
  json_ref enterPayload;

  ClientStateAssertion(
      const std::shared_ptr<watchman_root>& root,
      const w_string& name)
      : root(root), name(name) {}
};
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
  watchman::w_perf_t* perf_sample{nullptr};

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
  struct LoggedResponse {
    // TODO: also track the time when the response was enqueued
    std::chrono::system_clock::time_point written;
    json_ref response;
  };

  std::shared_ptr<watchman_root> root;
  w_string name;
  /* whether this subscription is paused */
  bool debug_paused = false;

  std::shared_ptr<w_query> query;
  bool vcs_defer;
  uint32_t last_sub_tick{0};
  // map of statename => bool.  If true, policy is drop, else defer
  std::unordered_map<w_string, bool> drop_or_defer;
  std::weak_ptr<watchman_client> weakClient;

  std::deque<LoggedResponse> lastResponses;

  explicit watchman_client_subscription(
      const std::shared_ptr<watchman_root>& root,
      std::weak_ptr<watchman_client> client);
  ~watchman_client_subscription();
  void processSubscription();

  std::shared_ptr<watchman_user_client> lockClient();
  json_ref buildSubscriptionResults(
      const std::shared_ptr<watchman_root>& root,
      ClockSpec& position,
      OnStateTransition onStateTransition);

 private:
  ClockSpec runSubscriptionRules(
      watchman_user_client* client,
      const std::shared_ptr<watchman_root>& root);
  void updateSubscriptionTicks(w_query_res* res);
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
