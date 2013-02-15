/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool subscription_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx,
    void *gendata)
{
  struct watchman_file *f;
  struct watchman_client_subscription *sub = gendata;

  w_log(W_LOG_DBG, "running subscription %s %p\n",
      sub->name->buf, sub);

  // Walk back in time until we hit the boundary
  for (f = root->latest_file; f; f = f->next) {
    if (sub->since.is_timestamp &&
        w_timeval_compare(f->otime.tv, sub->since.tv) < 0) {
      break;
    }
    if (!sub->since.is_timestamp &&
        f->otime.ticks <= sub->since.ticks) {
      break;
    }

    if (!w_query_process_file(query, ctx, f)) {
      return false;
    }
  }

  return true;
}

static json_t *build_subscription_results(
    struct watchman_client_subscription *sub,
    w_root_t *root)
{
  uint32_t num_results, ticks;
  struct watchman_rule_match *results = NULL;
  json_t *response;
  json_t *file_list;
  char clockbuf[128];

  w_log(W_LOG_DBG, "running subscription rules! since %" PRIu32 "\n",
      sub->since.ticks);

  num_results = w_query_execute(sub->query, root, &ticks, &results,
      subscription_generator, sub);

  w_log(W_LOG_DBG, "generated %" PRIu32 " results\n", num_results);

  file_list = w_query_results_to_json(&sub->field_list, num_results, results);
  w_match_results_free(num_results, results);

  if (num_results == 0) {
    return NULL;
  }

  response = make_response();

  if (clock_id_string(sub->since.ticks, clockbuf, sizeof(clockbuf))) {
    set_prop(response, "since", json_string_nocheck(clockbuf));
  }
  if (clock_id_string(ticks, clockbuf, sizeof(clockbuf))) {
    set_prop(response, "clock", json_string_nocheck(clockbuf));
  }
  sub->since.is_timestamp = false;
  sub->since.ticks = ticks;

  set_prop(response, "files", file_list);
  set_prop(response, "root", json_string(root->root_path->buf));
  set_prop(response, "subscription", json_string(sub->name->buf));

  return response;
}

/* must be called with root and client locked */
void w_run_subscription_rules(
    struct watchman_client *client,
    struct watchman_client_subscription *sub,
    w_root_t *root)
{
  json_t *response = build_subscription_results(sub, root);

  if (!response) {
    return;
  }

  if (!enqueue_response(client, response, true)) {
    w_log(W_LOG_DBG, "failed to queue sub response\n");
    json_decref(response);
  }
}

/* unsubscribe /root subname
 * Cancels a subscription */
void cmd_unsubscribe(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  const char *name;
  w_string_t *sname;
  bool deleted;
  json_t *resp;

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  name = json_string_value(json_array_get(args, 2));
  if (!name) {
    send_error_response(client,
        "expected 2nd parameter to be subscription name");
    return;
  }

  sname = w_string_new(name);

  pthread_mutex_lock(&w_client_lock);
  deleted = w_ht_del(client->subscriptions, (w_ht_val_t)sname);
  pthread_mutex_unlock(&w_client_lock);

  w_string_delref(sname);

  resp = make_response();
  set_prop(resp, "unsubscribe", json_string_nocheck(name));
  set_prop(resp, "deleted", json_boolean(deleted));

  send_and_dispose_response(client, resp);
}

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
void cmd_subscribe(struct watchman_client *client, json_t *args)
{
  w_root_t *root;
  struct watchman_client_subscription *sub;
  json_t *resp;
  const char *name;
  json_t *jfield_list;
  w_query *query;
  json_t *query_spec;
  struct w_query_field_list field_list;
  char *errmsg;

  if (json_array_size(args) != 4) {
    send_error_response(client, "wrong number of arguments for subscribe");
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  name = json_string_value(json_array_get(args, 2));
  if (!name) {
    send_error_response(client,
        "expected 2nd parameter to be subscription name");
    return;
  }

  query_spec = json_array_get(args, 3);

  jfield_list = json_object_get(query_spec, "fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(client, "invalid field list: %s", errmsg);
    free(errmsg);
    return;
  }

  query = w_query_parse(query_spec, &errmsg);
  if (!query) {
    send_error_response(client, "failed to parse query: %s", errmsg);
    free(errmsg);
    return;
  }

  sub = calloc(1, sizeof(*sub));
  if (!sub) {
    send_error_response(client, "no memory!");
    return;
  }

  sub->name = w_string_new(name);
  sub->query = query;
  memcpy(&sub->field_list, &field_list, sizeof(field_list));
  sub->root = root;

  /* special 'since' handling */
  if (query->since) {
    if (!w_parse_clockspec(root, query->since, &sub->since, true)) {
      memset(&sub->since, 0, sizeof(sub->since));
    }
  }

  pthread_mutex_lock(&w_client_lock);
  w_ht_replace(client->subscriptions, (w_ht_val_t)sub->name, (w_ht_val_t)sub);
  pthread_mutex_unlock(&w_client_lock);

  resp = make_response();
  annotate_with_clock(root, resp);
  set_prop(resp, "subscribe", json_string(name));
  send_and_dispose_response(client, resp);

  resp = build_subscription_results(sub, root);
  if (resp) {
    send_and_dispose_response(client, resp);
  }
}


/* vim:ts=2:sw=2:et:
 */

