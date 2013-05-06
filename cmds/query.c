/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"


/* query /root {query} */
void cmd_query(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  w_query *query;
  json_t *query_spec;
  char *errmsg = NULL;
  w_query_res res;
  json_t *response;
  json_t *file_list, *jfield_list;
  char clockbuf[128];
  struct w_query_field_list field_list;

  if (json_array_size(args) != 3) {
    send_error_response(client, "wrong number of arguments for 'query'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  query_spec = json_array_get(args, 2);

  jfield_list = json_object_get(query_spec, "fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(client, "invalid field list: %s", errmsg);
    free(errmsg);
    w_root_delref(root);
    return;
  }

  query = w_query_parse(query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    w_root_delref(root);
    return;
  }

  if (!w_query_execute(query, root, &res, NULL, NULL)) {
    send_error_response(client, "query failed: %s", res.errmsg);
    w_query_result_free(&res);
    w_root_delref(root);
    w_query_delref(query);
    return;
  }

  w_query_delref(query);

  file_list = w_query_results_to_json(&field_list,
                res.num_results, res.results);
  w_query_result_free(&res);

  response = make_response();
  if (clock_id_string(res.ticks, clockbuf, sizeof(clockbuf))) {
    set_prop(response, "clock", json_string_nocheck(clockbuf));
  }
  set_prop(response, "files", file_list);

  send_and_dispose_response(client, response);
  w_root_delref(root);
}


/* vim:ts=2:sw=2:et:
 */

