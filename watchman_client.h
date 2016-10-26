/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <unordered_map>

struct watchman_client_response {
  struct watchman_client_response *next;
  json_ref json;
};

struct watchman_client_subscription;

struct watchman_client_state_assertion {
  w_root_t *root; // Holds a ref on the root
  w_string name;
  long id;

  watchman_client_state_assertion(w_root_t* root, const w_string& name);
  ~watchman_client_state_assertion();
};

struct watchman_client {
  w_stm_t stm;
  w_evt_t ping;
  int log_level;
  w_jbuffer_t reader, writer;
  bool client_mode;
  bool client_is_owner;
  enum w_pdu_type pdu_type;

  // The command currently being processed by dispatch_command
  json_ref current_command;
  w_perf_t* perf_sample;

  // This handle is not joinable (CREATE_DETACHED), but can be
  // used to deliver signals.
  pthread_t thread_handle;

  struct watchman_client_response *head, *tail;
};

// These are approximations for managing derived client "classes"
extern void derived_client_dtor(struct watchman_client *client);
extern void derived_client_ctor(struct watchman_client *client);
extern const uint32_t derived_client_size;

struct watchman_client_subscription {
  w_root_t *root;
  w_string name;
  std::shared_ptr<w_query> query;
  bool vcs_defer;
  uint32_t last_sub_tick;
  struct w_query_field_list field_list;
  // map of statename => bool.  If true, policy is drop, else defer
  w_ht_t *drop_or_defer;

  ~watchman_client_subscription();
};

// Represents the server side session maintained for a client of
// the watchman per-user process
struct watchman_user_client {
  struct watchman_client client;

  /* map of subscription name => struct watchman_client_subscription */
  std::unordered_map<w_string, std::unique_ptr<watchman_client_subscription>>
      subscriptions;

  /* map of unique id => watchman_client_state_assertion */
  w_ht_t *states;
  long next_state_id;
};

extern pthread_mutex_t w_client_lock;
extern w_ht_t *clients;
void w_client_lock_init(void);

void w_client_vacate_states(struct watchman_user_client *client);
