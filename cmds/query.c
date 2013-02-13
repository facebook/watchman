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
  uint32_t num_results = 0;
  struct watchman_rule_match *results = NULL;
  uint32_t ticks = 0;
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

  query = w_query_parse(query_spec, &errmsg);
  jfield_list = json_object_get(query_spec, "fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(client, "invalid field list: %s", errmsg);
    free(errmsg);
    return;
  }

  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    return;
  }

  num_results = w_query_execute(query, root, &ticks, &results, NULL, NULL);
  w_query_delref(query);

  file_list = w_query_results_to_json(&field_list, num_results, results);
  w_match_results_free(num_results, results);

  response = make_response();
  if (clock_id_string(ticks, clockbuf, sizeof(clockbuf))) {
    set_prop(response, "clock", json_string_nocheck(clockbuf));
  }
  set_prop(response, "files", file_list);

  send_and_dispose_response(client, response);
}


/* vim:ts=2:sw=2:et:
 */

