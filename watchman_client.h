/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "watchman_synchronized.h"

struct watchman_client_subscription;

struct watchman_client_state_assertion {
  w_root_t *root; // Holds a ref on the root
  w_string name;
  long id;

  watchman_client_state_assertion(w_root_t* root, const w_string& name);
  ~watchman_client_state_assertion();
};

struct watchman_client {
  w_stm_t stm{nullptr};
  w_evt_t ping{nullptr};
  int log_level{0};
  w_jbuffer_t reader, writer;
  bool client_mode{false};
  bool client_is_owner{false};
  enum w_pdu_type pdu_type;

  // The command currently being processed by dispatch_command
  json_ref current_command;
  w_perf_t* perf_sample{nullptr};

  // This handle is not joinable (CREATE_DETACHED), but can be
  // used to deliver signals.
  pthread_t thread_handle;

  // Queue of things to send to the client.
  // Protected by clients.wlock()
  std::deque<json_ref> responses;

  virtual ~watchman_client();
};

struct watchman_client_subscription {
  w_root_t *root;
  w_string name;
  std::shared_ptr<w_query> query;
  bool vcs_defer;
  uint32_t last_sub_tick;
  struct w_query_field_list field_list;
  // map of statename => bool.  If true, policy is drop, else defer
  std::unordered_map<w_string, bool> drop_or_defer;
};

// Represents the server side session maintained for a client of
// the watchman per-user process
struct watchman_user_client : public watchman_client {
  /* map of subscription name => struct watchman_client_subscription */
  std::unordered_map<w_string, std::unique_ptr<watchman_client_subscription>>
      subscriptions;

  /* map of unique id => watchman_client_state_assertion.
   * The values are owned by root::asserted_states */
  std::unordered_map<long, watchman_client_state_assertion*> states;
  long next_state_id{0};

  ~watchman_user_client();
};

extern watchman::
    Synchronized<std::unordered_set<watchman_client*>, std::recursive_mutex>
        clients;

void w_client_vacate_states(struct watchman_user_client *client);
