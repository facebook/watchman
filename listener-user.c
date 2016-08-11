/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Functions relating to the per-user service

static void cmd_shutdown(struct watchman_client *client, json_t *args) {
  json_t *resp = make_response();
  unused_parameter(args);

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");
  w_request_shutdown();

  set_prop(resp, "shutdown-server", json_true());
  send_and_dispose_response(client, resp);
}
W_CMD_REG("shutdown-server", cmd_shutdown, CMD_DAEMON|CMD_POISON_IMMUNE, NULL)

void add_root_warnings_to_response(json_t *response, w_root_t *root) {
  char *str = NULL;
  char *full = NULL;

  if (!root->last_recrawl_reason && !root->warning) {
    return;
  }

  if (cfg_get_bool(root, "suppress_recrawl_warnings", false)) {
    return;
  }

  if (root->last_recrawl_reason) {
    ignore_result(
        asprintf(&str, "Recrawled this watch %d times, most recently because:\n"
                       "%.*s\n"
                       "To resolve, please review the information on\n"
                       "%s#recrawl",
                 root->recrawl_count, root->last_recrawl_reason->len,
                 root->last_recrawl_reason->buf, cfg_get_trouble_url()));
  }

  ignore_result(asprintf(
      &full,
      "%.*s%s" // root->warning
      "%s\n"   // str (last recrawl reason)
      "To clear this warning, run:\n"
      "`watchman watch-del %.*s ; watchman watch-project %.*s`\n",
      root->warning ? root->warning->len : 0,
      root->warning ? root->warning->buf : "",
      root->warning && str ? "\n" : "", // newline if we have both strings
      str ? str : "", root->root_path->len, root->root_path->buf,
      root->root_path->len, root->root_path->buf));

  if (full) {
    set_prop(response, "warning", typed_string_to_json(full, W_STRING_MIXED));
  }
  free(str);
  free(full);
}

bool resolve_root_or_err(struct watchman_client *client, json_t *args,
                         int root_index, bool create,
                         struct unlocked_watchman_root *unlocked) {
  const char *root_name;
  char *errmsg = NULL;
  json_t *ele;

  unlocked->root = NULL;

  ele = json_array_get(args, root_index);
  if (!ele) {
    send_error_response(client, "wrong number of arguments");
    return false;
  }

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(client, "invalid value for argument %d, expected "
                                "a string naming the root dir",
                        root_index);
    return false;
  }

  if (client->client_mode) {
    w_root_resolve_for_client_mode(root_name, &errmsg, unlocked);
  } else {
    if (!client->client_is_owner) {
      // Only the owner is allowed to create watches
      create = false;
    }
    w_root_resolve(root_name, create, &errmsg, unlocked);
  }

  if (!unlocked->root) {
    if (!client->client_is_owner) {
      send_error_response(client, "unable to resolve root %s: %s (this may be "
                                  "because you are not the process owner)",
                          root_name, errmsg);
    } else {
      send_error_response(client, "unable to resolve root %s: %s", root_name,
                          errmsg);
    }
    free(errmsg);
    return false;
  }

  w_perf_add_root_meta(&client->perf_sample, unlocked->root);
  return true;
}

static void delete_subscription(w_ht_val_t val)
{
  struct watchman_client_subscription *sub = w_ht_val_ptr(val);

  w_string_delref(sub->name);
  w_query_delref(sub->query);
  if (sub->drop_or_defer) {
    w_ht_free(sub->drop_or_defer);
  }
  free(sub);
}

static const struct watchman_hash_funcs subscription_hash_funcs = {
    w_ht_string_copy,
    w_ht_string_del,
    w_ht_string_equal,
    w_ht_string_hash,
    NULL,
    delete_subscription};

void derived_client_ctor(struct watchman_client *ptr) {
  struct watchman_user_client *client = (struct watchman_user_client *)ptr;

  client->subscriptions = w_ht_new(2, &subscription_hash_funcs);
}

void derived_client_dtor(struct watchman_client *ptr) {
  struct watchman_user_client *client = (struct watchman_user_client *)ptr;

  /* cancel subscriptions */
  w_ht_free(client->subscriptions);
  client->subscriptions = NULL;

  w_client_vacate_states(client);
}
const uint32_t derived_client_size = sizeof(struct watchman_user_client);

/* vim:ts=2:sw=2:et:
 */
