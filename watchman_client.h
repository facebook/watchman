/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "Logging.h"
#include "watchman_synchronized.h"

struct watchman_client_subscription;

struct watchman_client_state_assertion {
  std::shared_ptr<w_root_t> root; // Holds a ref on the root
  w_string name;
  long id;

  watchman_client_state_assertion(
      const std::shared_ptr<w_root_t>& root,
      const w_string& name);
};

struct watchman_client : public std::enable_shared_from_this<watchman_client> {
  std::unique_ptr<watchman_stream> stm;
  std::unique_ptr<watchman_event> ping;
  w_jbuffer_t reader, writer;
  bool client_mode{false};
  bool client_is_owner{false};
  enum w_pdu_type pdu_type;
  uint32_t capabilities;

  // The command currently being processed by dispatch_command
  json_ref current_command;
  w_perf_t* perf_sample{nullptr};

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

struct watchman_client_subscription
    : public std::enable_shared_from_this<watchman_client_subscription> {
  std::shared_ptr<w_root_t> root;
  w_string name;
  /* whether this subscription is paused */
  bool debug_paused = false;

  std::shared_ptr<w_query> query;
  bool vcs_defer;
  uint32_t last_sub_tick;
  // map of statename => bool.  If true, policy is drop, else defer
  std::unordered_map<w_string, bool> drop_or_defer;
  std::weak_ptr<watchman_client> weakClient;

  explicit watchman_client_subscription(
      const std::shared_ptr<w_root_t>& root,
      std::weak_ptr<watchman_client> client);
  ~watchman_client_subscription();
  void processSubscription();

  std::shared_ptr<watchman_user_client> lockClient();
  json_ref buildSubscriptionResults(
      const std::shared_ptr<w_root_t>& root,
      ClockSpec& position);

 private:
  ClockSpec runSubscriptionRules(
      watchman_user_client* client,
      const std::shared_ptr<w_root_t>& root);
  void updateSubscriptionTicks(w_query_res* res);
};

// Represents the server side session maintained for a client of
// the watchman per-user process
struct watchman_user_client : public watchman_client {
  /* map of subscription name => struct watchman_client_subscription */
  std::unordered_map<w_string, std::shared_ptr<watchman_client_subscription>>
      subscriptions;

  /* map of unique id => watchman_client_state_assertion.
   * The values are owned by root::asserted_states */
  std::unordered_map<long, std::weak_ptr<watchman_client_state_assertion>>
      states;
  long next_state_id{0};

  // Subscriber to root::unilateralResponses
  std::unordered_map<
      std::shared_ptr<watchman_client_subscription>,
      std::shared_ptr<watchman::Publisher::Subscriber>>
      unilateralSub;

  explicit watchman_user_client(std::unique_ptr<watchman_stream>&& stm);
  ~watchman_user_client() override;

  bool unsubByName(const w_string& name);
};

extern watchman::Synchronized<
    std::unordered_set<std::shared_ptr<watchman_client>>>
    clients;

void w_client_vacate_states(struct watchman_user_client *client);
