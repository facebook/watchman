/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct state_arg {
  w_string_t *name;
  int sync_timeout;
  json_ref metadata;
};

// Parses the args for state-enter and state-leave
static bool parse_state_arg(
    struct watchman_client* client,
    const json_ref& args,
    struct state_arg* parsed) {
  json_error_t err;
  const char *ignored;
  const char* statename = nullptr;

  parsed->sync_timeout = DEFAULT_QUERY_SYNC_MS;
  parsed->metadata = nullptr;
  parsed->name = nullptr;

  if (json_array_size(args) != 3) {
    send_error_response(client,
        "invalid number of arguments, expected 3, got %d",
        json_array_size(args));
    return false;
  }

  // [cmd, root, statename]
  if (json_unpack_ex(args, &err, 0, "[ssu]",
        &ignored, &ignored, &statename) == 0) {
    parsed->name = w_string_new_typed(statename, W_STRING_UNICODE);
    return true;
  }

  if (json_unpack_ex(args, &err, 0, "[s, s, {s:u, s?:O, s?:i}]",
        &ignored, &ignored,
        "name", &statename,
        "metadata", &parsed->metadata,
        "sync_timeout", &parsed->sync_timeout) != 0) {
    send_error_response(client, "invalid arguments: %s", err.text);
    return false;
  }

  if (parsed->sync_timeout < 0) {
    send_error_response(client, "sync_timeout must be >= 0");
    return false;
  }

  parsed->name = w_string_new_typed(statename, W_STRING_UNICODE);
  return true;
}

static void destroy_state_arg(struct state_arg *parsed) {
  if (parsed->name) {
    w_string_delref(parsed->name);
  }
  parsed->metadata.reset();
  memset(parsed, 0, sizeof(*parsed));
}

watchman_client_state_assertion::watchman_client_state_assertion(
    w_root_t* root,
    const w_string& name)
    : root(root), name(name), id(0) {
  w_root_addref(root);
}

watchman_client_state_assertion::~watchman_client_state_assertion() {
  w_root_delref_raw(root);
}

static void cmd_state_enter(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed = {nullptr, 0, nullptr};
  std::unique_ptr<watchman_client_state_assertion> assertion;
  char clockbuf[128];
  w_ht_iter_t iter;
  json_ref response;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout &&
      !w_root_sync_to_now(&unlocked, parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    goto done;
  }

  assertion.reset(
      new watchman_client_state_assertion(unlocked.root, parsed.name));
  if (!assertion) {
    send_error_response(client, "out of memory");
    goto done;
  }

  {
    auto wlock = unlocked.root->asserted_states.wlock();
    auto& map = *wlock;
    auto& entry = map[assertion->name];

    // If the state is already asserted, we can't re-assert it
    if (entry) {
      send_error_response(
          client, "state %s is already asserted", parsed.name->buf);
      goto done;
    }
    // We're now in this state
    entry = std::move(assertion);

    // Record the state assertion in the client
    if (!client->states) {
      client->states = w_ht_new(2, NULL);
    }
    entry->id = ++client->next_state_id;
    w_ht_set(client->states, entry->id, w_ht_ptr_val(entry.get()));
  }

  w_root_read_lock(&unlocked, "state-enter", &lock);
  {
    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(
        lock.root->inner.number,
        lock.root->inner.ticks,
        clockbuf,
        sizeof(clockbuf));
  }
  w_root_read_unlock(&lock, &unlocked);

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  response.set({{"root", w_string_to_json(unlocked.root->root_path)},
                {"state-enter", w_string_to_json(parsed.name)},
                {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)}});
  send_and_dispose_response(client, std::move(response));

  // Now find all the clients with subscriptions and send them
  // notice of the state being entered
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    auto subclient = (watchman_user_client*)w_ht_val_ptr(iter.value);

    for (auto& citer : subclient->subscriptions) {
      auto sub = citer.second.get();

      if (sub->root != unlocked.root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }

      auto pdu = make_response();
      pdu.set({{"root", w_string_to_json(unlocked.root->root_path)},
               {"subscription", w_string_to_json(sub->name)},
               {"unilateral", json_true()},
               {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)},
               {"state-enter", w_string_to_json(parsed.name)}});
      if (parsed.metadata) {
        pdu.set("metadata", json_ref(parsed.metadata));
      }
      enqueue_response(subclient, std::move(pdu), true);
    }
  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

done:
  w_root_delref(&unlocked);
  destroy_state_arg(&parsed);
}
W_CMD_REG("state-enter", cmd_state_enter, CMD_DAEMON, w_cmd_realpath_root)

static void leave_state(struct watchman_user_client *client,
    struct watchman_client_state_assertion *assertion,
    bool abandoned, json_t *metadata, const char *clockbuf) {
  char buf[128];
  w_ht_iter_t iter;
  struct unlocked_watchman_root unlocked = {assertion->root};

  if (!clockbuf) {
    struct read_locked_watchman_root lock;
    w_root_read_lock(&unlocked, "state-leave", &lock);
    clock_id_string(
        lock.root->inner.number, lock.root->inner.ticks, buf, sizeof(buf));
    w_root_read_unlock(&lock, &unlocked);

    clockbuf = buf;
  }

  // First locate all subscribers and notify them
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    auto subclient = (watchman_user_client*)w_ht_val_ptr(iter.value);

    for (auto& citer : subclient->subscriptions) {
      auto sub = citer.second.get();

      if (sub->root != unlocked.root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }

      auto pdu = make_response();
      pdu.set({{"root", w_string_to_json(unlocked.root->root_path)},
               {"subscription", w_string_to_json(sub->name)},
               {"unilateral", json_true()},
               {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)},
               {"state-leave", w_string_to_json(assertion->name)}});
      if (metadata) {
        pdu.set("metadata", json_ref(metadata));
      }
      if (abandoned) {
        pdu.set("abandoned", json_true());
      }
      enqueue_response(subclient, std::move(pdu), true);
    }
  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

  // The erase will delete the assertion pointer, so save these things
  auto id = assertion->id;
  w_string name = assertion->name;

  // Now remove the state
  {
    auto map = unlocked.root->asserted_states.wlock();
    map->erase(name);
  }

  if (client) {
    w_ht_del(client->states, id);
  }
}

// Abandon any states that haven't been explicitly vacated
void w_client_vacate_states(struct watchman_user_client *client) {
  w_ht_iter_t iter;

  if (!client->states) {
    return;
  }

  while (w_ht_first(client->states, &iter)) {
    w_root_t *root;

    auto assertion = (watchman_client_state_assertion*)w_ht_val_ptr(iter.value);
    root = assertion->root;

    w_log(
        W_LOG_ERR,
        "implicitly vacating state %s on %s due to client disconnect\n",
        assertion->name.c_str(),
        root->root_path.c_str());

    // This will delete the state from client->states and invalidate
    // the iterator.
    leave_state(client, assertion, true, NULL, NULL);
  }

  w_ht_free(client->states);
}

static void cmd_state_leave(
    struct watchman_client* clientbase,
    const json_ref& args) {
  struct state_arg parsed = {nullptr, 0, nullptr};
  // This is a weak reference to the assertion.  This is safe because only this
  // client can delete this assertion, and this function is only executed by
  // the thread that owns this client.
  struct watchman_client_state_assertion *assertion = NULL;
  char clockbuf[128];
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct read_locked_watchman_root lock;
  struct unlocked_watchman_root unlocked;
  json_ref response;

  if (!resolve_root_or_err(client, args, 1, true, &unlocked)) {
    return;
  }

  if (!parse_state_arg(client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout &&
      !w_root_sync_to_now(&unlocked, parsed.sync_timeout)) {
    send_error_response(client, "synchronization failed: %s", strerror(errno));
    goto done;
  }

  {
    auto map = unlocked.root->asserted_states.rlock();
    // Confirm that this client owns this state
    const auto& it = map->find(parsed.name);
    // If the state is not asserted, we can't leave it
    if (it == map->end()) {
      send_error_response(client, "state %s is not asserted", parsed.name->buf);
      goto done;
    }

    assertion = it->second.get();

    // Sanity check ownership
    if (w_ht_val_ptr(w_ht_get(client->states, assertion->id)) != assertion) {
      send_error_response(
          client,
          "state %s was not asserted by this session",
          parsed.name->buf);
      goto done;
    }
  }

  w_root_read_lock(&unlocked, "state-leave", &lock);
  {
    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(
        lock.root->inner.number,
        lock.root->inner.ticks,
        clockbuf,
        sizeof(clockbuf));
  }
  w_root_read_unlock(&lock, &unlocked);

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  response.set({{"root", w_string_to_json(unlocked.root->root_path)},
                {"state-leave", w_string_to_json(parsed.name)},
                {"clock", typed_string_to_json(clockbuf, W_STRING_UNICODE)}});
  send_and_dispose_response(client, std::move(response));

  // Notify and exit the state
  leave_state(client, assertion, false, parsed.metadata, clockbuf);

done:
  w_root_delref(&unlocked);
  destroy_state_arg(&parsed);
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
