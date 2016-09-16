/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* query /root {query} */
static void cmd_query(struct watchman_client *client, json_t *args)
{
  w_query *query;
  json_t *query_spec;
  char *errmsg = NULL;
  w_query_res res;
  json_t *response;
  json_t *file_list, *jfield_list;
  char clockbuf[128];
  struct w_query_field_list field_list;
  struct unlocked_watchman_root unlocked;

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments for 'query'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  query_spec = json_array_get(args, 2);

  jfield_list = json_object_get(query_spec, "fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(client, "invalid field list: %s", errmsg);
    free(errmsg);
    w_root_delref(&unlocked);
    return;
  }

  query = w_query_parse(unlocked.root, query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    w_root_delref(&unlocked);
    return;
  }

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
W_CMD_REG("query", cmd_query, CMD_DAEMON | CMD_CLIENT | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
