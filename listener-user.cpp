/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Functions relating to the per-user service

static void cmd_shutdown(struct watchman_client* client, const json_ref&) {
  auto resp = make_response();

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");
  w_request_shutdown();

  resp.set("shutdown-server", json_true());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("shutdown-server", cmd_shutdown, CMD_DAEMON|CMD_POISON_IMMUNE, NULL)

void add_root_warnings_to_response(
    json_ref& response,
    struct read_locked_watchman_root* lock) {
  char *str = NULL;
  char *full = NULL;
  const w_root_t *root = lock->root;
  auto info = lock->root->recrawlInfo.rlock();

  if (!info->lastRecrawlReason && !info->warning) {
    return;
  }

  if (root->config.getBool("suppress_recrawl_warnings", false)) {
    return;
  }

  if (info->lastRecrawlReason) {
    ignore_result(asprintf(
        &str,
        "Recrawled this watch %d times, most recently because:\n"
        "%s\n"
        "To resolve, please review the information on\n"
        "%s#recrawl",
        info->recrawlCount,
        info->lastRecrawlReason.c_str(),
        cfg_get_trouble_url()));
  }

  ignore_result(asprintf(
      &full,
      "%s%s" // root->warning
      "%s\n" // str (last recrawl reason)
      "To clear this warning, run:\n"
      "`watchman watch-del %s ; watchman watch-project %s`\n",
      info->warning ? info->warning.c_str() : "",
      info->warning && str ? "\n" : "", // newline if we have both strings
      str ? str : "",
      root->root_path.c_str(),
      root->root_path.c_str()));

  if (full) {
    response.set("warning", typed_string_to_json(full, W_STRING_MIXED));
  }
  free(str);
  free(full);
}

bool resolve_root_or_err(
    struct watchman_client* client,
    const json_ref& args,
    size_t root_index,
    bool create,
    struct unlocked_watchman_root* unlocked) {
  const char *root_name;
  char *errmsg = NULL;

  unlocked->root = NULL;

  if (args.array().size() <= root_index) {
    send_error_response(client, "wrong number of arguments");
    return false;
  }

  const auto& ele = args.at(root_index);

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

  if (client->perf_sample) {
    client->perf_sample->add_root_meta(unlocked->root);
  }
  return true;
}

watchman_user_client::~watchman_user_client() {
  /* cancel subscriptions */
  subscriptions.clear();

  w_client_vacate_states(this);
}

/* vim:ts=2:sw=2:et:
 */
