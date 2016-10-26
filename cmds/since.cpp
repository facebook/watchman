/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* since /root <timestamp> [patterns] */
static void cmd_since(struct watchman_client* client, const json_ref& args) {
  const char *clockspec;
  char *errmsg = NULL;
  struct w_query_field_list field_list;
  w_query_res res;
  char clockbuf[128];
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments for 'since'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  auto clock_ele = json_array_get(args, 2);
  clockspec = json_string_value(clock_ele);
  if (!clockspec) {
    send_error_response(client,
        "expected argument 2 to be a valid clockspec");
    w_root_delref(&unlocked);
    return;
  }

  auto query = w_query_parse_legacy(
      unlocked.root, args, &errmsg, 3, nullptr, clockspec, nullptr);
  if (errmsg) {
    send_error_response(client, "%s", errmsg);
    free(errmsg);
    w_root_delref(&unlocked);
    return;
  }

  w_query_legacy_field_list(&field_list);

  if (!w_query_execute(query.get(), &unlocked, &res, nullptr)) {
    send_error_response(client, "query failed: %s", res.errmsg);
    w_root_delref(&unlocked);
    return;
  }

  auto file_list =
      w_query_results_to_json(&field_list, res.results.size(), res.results);

  auto response = make_response();
  if (clock_id_string(res.root_number, res.ticks, clockbuf, sizeof(clockbuf))) {
    response.set("clock", typed_string_to_json(clockbuf, W_STRING_UNICODE));
  }
  response.set({{"is_fresh_instance", json_boolean(res.is_fresh_instance)},
                {"files", std::move(file_list)}});

  {
    struct read_locked_watchman_root lock;
    w_root_read_lock(&unlocked, "obtain_warnings", &lock);
    add_root_warnings_to_response(response, &lock);
    w_root_read_unlock(&lock, &unlocked);
  }

  send_and_dispose_response(client, std::move(response));
  w_root_delref(&unlocked);
}
W_CMD_REG("since", cmd_since, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
