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
    const std::shared_ptr<w_root_t>& root) {
  auto info = root->recrawlInfo.rlock();

  if (!info->warning) {
    return;
  }

  response.set(
      "warning",
      w_string_to_json(w_string::build(
          info->warning,
          "\n",
          "To clear this warning, run:\n"
          "`watchman watch-del ",
          root->root_path,
          " ; watchman watch-project ",
          root->root_path,
          "`\n")));
}

std::shared_ptr<w_root_t> resolve_root_or_err(
    struct watchman_client* client,
    const json_ref& args,
    size_t root_index,
    bool create) {
  const char *root_name;
  char *errmsg = NULL;

  if (args.array().size() <= root_index) {
    send_error_response(client, "wrong number of arguments");
    return nullptr;
  }

  const auto& ele = args.at(root_index);

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(
        client,
        "invalid value for argument %" PRIsize_t
        ", expected a string naming the root dir",
        root_index);
    return nullptr;
  }

  std::shared_ptr<w_root_t> root;
  if (client->client_mode) {
    root = w_root_resolve_for_client_mode(root_name, &errmsg);
  } else {
    if (!client->client_is_owner) {
      // Only the owner is allowed to create watches
      create = false;
    }
    root = w_root_resolve(root_name, create, &errmsg);
  }

  if (!root) {
    if (!client->client_is_owner) {
      send_error_response(client, "unable to resolve root %s: %s (this may be "
                                  "because you are not the process owner)",
                          root_name, errmsg);
    } else {
      send_error_response(client, "unable to resolve root %s: %s", root_name,
                          errmsg);
    }
    free(errmsg);
    return nullptr;
  }

  if (client->perf_sample) {
    client->perf_sample->add_root_meta(root);
  }
  return root;
}

watchman_user_client::watchman_user_client(
    std::unique_ptr<watchman_stream>&& stm)
    : watchman_client(std::move(stm)) {}

watchman_user_client::~watchman_user_client() {
  /* cancel subscriptions */
  subscriptions.clear();

  w_client_vacate_states(this);
}

/* vim:ts=2:sw=2:et:
 */
