/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* find /root [patterns] */
static void cmd_find(struct watchman_client *client, json_t *args)
{
  w_query *query;
  char *errmsg = NULL;
  struct w_query_field_list field_list;
  w_query_res res;
  json_t *response;
  json_t *file_list;
  char clockbuf[128];
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  query =
      w_query_parse_legacy(unlocked.root, args, &errmsg, 2, NULL, NULL, NULL);
  if (errmsg) {
    send_error_response(client, "%s", errmsg);
    free(errmsg);
    w_root_delref(&unlocked);
    return;
  }

  w_query_legacy_field_list(&field_list);

  if (client->client_mode) {
    query->sync_timeout = 0;
  }

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
  set_prop(response, "files", file_list);

  send_and_dispose_response(client, response);
  w_root_delref(&unlocked);
}
W_CMD_REG("find", cmd_find, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
