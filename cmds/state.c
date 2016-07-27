/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

struct state_arg {
  w_string_t *name;
  int sync_timeout;
  json_t *metadata;
};

// Parses the args for state-enter and state-leave
static bool parse_state_arg(struct watchman_client *client, json_t *args,
    struct state_arg *parsed) {
  json_error_t err;
  const char *ignored;
  const char *statename = NULL;

  parsed->sync_timeout = DEFAULT_QUERY_SYNC_MS;
  parsed->metadata = NULL;
  parsed->name = NULL;

  if (json_array_size(args) != 3) {
    send_error_response(client,
        "invalid number of arguments, expected 3, got %d",
        json_array_size(args));
    return false;
  }

  // [cmd, root, statename]
  if (json_unpack_ex(args, &err, 0, "[sss]",
        &ignored, &ignored, &statename) == 0) {
    parsed->name = w_string_new_typed(statename, W_STRING_UNICODE);
    return true;
  }

  if (json_unpack_ex(args, &err, 0, "[s, s, {s:s, s?:O, s?:i}]",
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
  if (parsed->metadata) {
    json_decref(parsed->metadata);
  }
  memset(parsed, 0, sizeof(*parsed));
}

static void free_assertion(struct watchman_client_state_assertion *assertion) {
  w_root_delref(assertion->root);
  w_string_delref(assertion->name);
}

static void cmd_state_enter(struct watchman_client *clientbase, json_t *args) {
  w_root_t *root;
  struct state_arg parsed = { NULL, 0, NULL };
  struct watchman_client_state_assertion *assertion = NULL;
  char clockbuf[128];
  w_ht_iter_t iter;
  json_t *response;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;

  root = resolve_root_or_err(&client->client, args, 1, true);
  if (!root) {
    return;
  }

  if (!parse_state_arg(&client->client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout && !w_root_sync_to_now(root, parsed.sync_timeout)) {
    send_error_response(&client->client, "synchronization failed: %s",
        strerror(errno));
    goto done;
  }

  assertion = calloc(1, sizeof(*assertion));
  if (!assertion) {
    send_error_response(&client->client, "out of memory");
    goto done;
  }

  assertion->root = root;
  assertion->name = parsed.name;
  w_root_addref(assertion->root);
  w_string_addref(assertion->name);

  w_root_lock(root, "state-enter");
  {
    // If the state is already asserted, we can't re-assert it
    if (!root->asserted_states) {
      root->asserted_states = w_ht_new(2, &w_ht_string_funcs);
    } else if (w_ht_get(root->asserted_states, w_ht_ptr_val(parsed.name))) {
      send_error_response(&client->client, "state %s is already asserted",
          parsed.name->buf);
      w_root_unlock(root);
      goto done;
    }

    // We're now in this state
    w_ht_set(root->asserted_states, w_ht_ptr_val(parsed.name),
        w_ht_ptr_val(assertion));

    // Record the state assertion in the client
    if (!client->states) {
      client->states = w_ht_new(2, NULL);
    }
    w_refcnt_add(&client->next_state_id);
    assertion->id = client->next_state_id;
    w_ht_set(client->states, assertion->id, w_ht_ptr_val(assertion));

    // Make sure we don't free the assertion in the cleanup code below
    assertion = NULL;

    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(root->number, root->ticks, clockbuf, sizeof(clockbuf));
  }
  w_root_unlock(root);

  // We successfully entered the state, this is our response to the
  // state-enter command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  set_prop(response, "root", w_string_to_json(root->root_path));
  set_prop(response, "state-enter", w_string_to_json(parsed.name));
  set_prop(response, "clock", json_string_nocheck(clockbuf));
  send_and_dispose_response(&client->client, response);
  response = NULL;

  // Now find all the clients with subscriptions and send them
  // notice of the state being entered
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_user_client *subclient = w_ht_val_ptr(iter.value);
    w_ht_iter_t citer;

    if (w_ht_first(subclient->subscriptions, &citer)) do {
      struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);
      json_t *pdu;

      if (sub->root != root) {
        w_log(W_LOG_DBG, "root doesn't match, skipping\n");
        continue;
      }

      pdu = make_response();
      set_prop(pdu, "root", w_string_to_json(root->root_path));
      set_prop(pdu, "subscription", w_string_to_json(sub->name));
      set_prop(pdu, "unilateral", json_true());
      set_prop(pdu, "clock", json_string_nocheck(clockbuf));
      set_prop(pdu, "state-enter", w_string_to_json(parsed.name));
      if (parsed.metadata) {
        // set_prop would steal our ref, we don't want that
        json_object_set_nocheck(pdu, "metadata", parsed.metadata);
      }
      if (!enqueue_response(&subclient->client, pdu, true)) {
        json_decref(pdu);
      }

    } while (w_ht_next(subclient->subscriptions, &citer));
  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

done:
  w_root_delref(root);
  destroy_state_arg(&parsed);
  if (assertion) {
    free_assertion(assertion);
  }
}
W_CMD_REG("state-enter", cmd_state_enter, CMD_DAEMON, w_cmd_realpath_root)

static void leave_state(struct watchman_user_client *client,
    struct watchman_client_state_assertion *assertion,
    bool abandoned, json_t *metadata, const char *clockbuf) {
  char buf[128];
  w_ht_iter_t iter;
  w_root_t *root = assertion->root;

  if (!clockbuf) {
    w_root_lock(root, "state-leave");
    clock_id_string(root->number, root->ticks, buf, sizeof(buf));
    w_root_unlock(root);

    clockbuf = buf;
  }

  // First locate all subscribers and notify them
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_user_client *subclient = w_ht_val_ptr(iter.value);
    w_ht_iter_t citer;

    if (subclient->subscriptions &&
        w_ht_first(subclient->subscriptions, &citer))
      do {
        struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);
        json_t *pdu;

        if (sub->root != root) {
          w_log(W_LOG_DBG, "root doesn't match, skipping\n");
          continue;
        }

        pdu = make_response();
        set_prop(pdu, "root", w_string_to_json(root->root_path));
        set_prop(pdu, "subscription", w_string_to_json(sub->name));
        set_prop(pdu, "unilateral", json_true());
        set_prop(pdu, "clock", json_string_nocheck(clockbuf));
        set_prop(pdu, "state-leave", w_string_to_json(assertion->name));
        if (metadata) {
          // set_prop would steal our ref, we don't want that
          json_object_set_nocheck(pdu, "metadata", metadata);
        }
        if (abandoned) {
          set_prop(pdu, "abandoned", json_true());
        }
        if (!enqueue_response(&subclient->client, pdu, true)) {
          json_decref(pdu);
        }

      } while (w_ht_next(subclient->subscriptions, &citer));
  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);

  // Now remove the state
  w_root_lock(root, "state-leave");
  w_ht_del(root->asserted_states, w_ht_ptr_val(assertion->name));
  w_root_unlock(root);

  if (client) {
    w_ht_del(client->states, assertion->id);
  }
  free_assertion(assertion);
}

// Abandon any states that haven't been explicitly vacated
void w_client_vacate_states(struct watchman_user_client *client) {
  w_ht_iter_t iter;

  if (!client->states) {
    return;
  }

  while (w_ht_first(client->states, &iter)) {
    struct watchman_client_state_assertion *assertion;
    w_root_t *root;

    assertion = w_ht_val_ptr(iter.value);
    root = assertion->root;

    w_log(W_LOG_ERR,
        "implicitly vacating state %s on %.*s due to client disconnect\n",
        assertion->name->buf,
        root->root_path->len, root->root_path->buf);

    // This will delete the state from client->states and invalidate
    // the iterator.
    leave_state(client, assertion, true, NULL, NULL);
  }
}

static void cmd_state_leave(struct watchman_client *clientbase, json_t *args) {
  w_root_t *root;
  struct state_arg parsed = { NULL, 0, NULL };
  struct watchman_client_state_assertion *assertion = NULL;
  char clockbuf[128];
  json_t *response;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;

  root = resolve_root_or_err(&client->client, args, 1, true);
  if (!root) {
    return;
  }

  if (!parse_state_arg(&client->client, args, &parsed)) {
    goto done;
  }

  if (parsed.sync_timeout && !w_root_sync_to_now(root, parsed.sync_timeout)) {
    send_error_response(&client->client, "synchronization failed: %s",
        strerror(errno));
    goto done;
  }

  // Confirm that this client owns this state
  w_root_lock(root, "state-leave");
  {
    assertion = root->asserted_states ?
          w_ht_val_ptr(w_ht_get(root->asserted_states,
                w_ht_ptr_val(parsed.name)))
          : NULL;

    // If the state is not asserted, we can't leave it
    if (!assertion) {
      send_error_response(&client->client, "state %s is not asserted",
          parsed.name->buf);
      w_root_unlock(root);
      goto done;
    }

    // Sanity check ownership
    if (w_ht_val_ptr(w_ht_get(client->states, assertion->id)) != assertion) {
      send_error_response(&client->client,
                          "state %s was not asserted by this session",
                          parsed.name->buf);
      w_root_unlock(root);
      goto done;
    }

    // Sample the clock buf for the subscription PDUs we're going to
    // send
    clock_id_string(root->number, root->ticks, clockbuf, sizeof(clockbuf));
  }
  w_root_unlock(root);

  // We're about to successfully leave the state, this is our response to the
  // state-leave command.  We do this before we send the subscription
  // PDUs in case CLIENT has active subscriptions for this root
  response = make_response();
  set_prop(response, "root", w_string_to_json(root->root_path));
  set_prop(response, "state-leave", w_string_to_json(parsed.name));
  set_prop(response, "clock", json_string_nocheck(clockbuf));
  send_and_dispose_response(&client->client, response);
  response = NULL;

  // Notify and exit the state
  leave_state(client, assertion, false, parsed.metadata, clockbuf);

done:
  w_root_delref(root);
  destroy_state_arg(&parsed);
}
W_CMD_REG("state-leave", cmd_state_leave, CMD_DAEMON, w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
