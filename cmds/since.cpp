/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* since /root <timestamp> [patterns] */
static void cmd_since(struct watchman_client *client, json_t *args)
{
  const char *clockspec;
  w_query *query;
  char *errmsg = NULL;
  struct w_query_field_list field_list;
  w_query_res res;
  json_t *response, *clock_ele;
  json_t *file_list;
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

  clock_ele = json_array_get(args, 2);
  clockspec = json_string_value(clock_ele);
  if (!clockspec) {
    send_error_response(client,
        "expected argument 2 to be a valid clockspec");
    w_root_delref(&unlocked);
    return;
  }

  query = w_query_parse_legacy(unlocked.root, args, &errmsg, 3, NULL, clockspec,
                               NULL);
  if (errmsg) {
    send_error_response(client, "%s", errmsg);
    free(errmsg);
    w_root_delref(&unlocked);
    return;
  }

  w_query_legacy_field_list(&field_list);

  if (!w_query_execute(query, &unlocked, &res, NULL, NULL)) {
    send_error_response(client, "query failed: %s", res.errmsg);
    w_query_result_free(&res);
    w_root_delref(&unlocked);
    w_query_delref(query);
    return;
  }

  w_query_delref(query);

  file_list = w_query_results_to_json(&field_list,
                res.num_results, res.results);
  w_query_result_free(&res);

  response = make_response();
  if (clock_id_string(res.root_number, res.ticks, clockbuf, sizeof(clockbuf))) {
    set_unicode_prop(response, "clock", clockbuf);
  }
  set_prop(response, "is_fresh_instance",
           json_pack("b", res.is_fresh_instance));
  set_prop(response, "files", file_list);

  {
    struct read_locked_watchman_root lock;
    w_root_read_lock(&unlocked, "obtain_warnings", &lock);
    add_root_warnings_to_response(response, &lock);
    w_root_read_unlock(&lock, &unlocked);
  }

  send_and_dispose_response(client, response);
  w_root_delref(&unlocked);
}
W_CMD_REG("since", cmd_since, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
