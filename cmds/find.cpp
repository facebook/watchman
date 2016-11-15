/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* find /root [patterns] */
static void cmd_find(struct watchman_client* client, const json_ref& args) {
  char *errmsg = NULL;
  w_query_res res;

  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  auto query = w_query_parse_legacy(root, args, &errmsg, 2, NULL, NULL, NULL);
  if (errmsg) {
    send_error_response(client, "%s", errmsg);
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
  response.set(
      {{"clock", w_string_to_json(res.clockAtStartOfQuery.toClockString())},
       {"files", std::move(res.resultsArray)}});

  send_and_dispose_response(client, std::move(response));
}
W_CMD_REG("find", cmd_find, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
