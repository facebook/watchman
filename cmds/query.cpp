/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* query /root {query} */
static void cmd_query(struct watchman_client* client, const json_ref& args) {
  char *errmsg = NULL;
  w_query_res res;
  char clockbuf[128];

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments for 'query'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  const auto& query_spec = args.at(2);
  auto query = w_query_parse(root, query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    return;
  }

  if (client->client_mode) {
    query->sync_timeout = std::chrono::milliseconds(0);
  }

  if (!w_query_execute(query.get(), root, &res, nullptr)) {
    send_error_response(client, "query failed: %s", res.errmsg);
    return;
  }

  auto response = make_response();
  if (clock_id_string(res.root_number, res.ticks, clockbuf, sizeof(clockbuf))) {
    response.set("clock", typed_string_to_json(clockbuf, W_STRING_UNICODE));
  }
  response.set({{"is_fresh_instance", json_boolean(res.is_fresh_instance)},
                {"files", std::move(res.resultsArray)}});

  add_root_warnings_to_response(response, root);

  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG("query", cmd_query, CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
