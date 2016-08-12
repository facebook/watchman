/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static bool subscription_generator(w_query *query,
                                   struct read_locked_watchman_root *lock,
                                   struct w_query_ctx *ctx, void *gendata,
                                   int64_t *num_walked) {
  struct watchman_file *f;
  struct watchman_client_subscription *sub = gendata;
  int64_t n = 0;
  bool result = true;

  w_log(W_LOG_DBG, "running subscription %s %p\n",
      sub->name->buf, sub);

  // Walk back in time until we hit the boundary
  for (f = lock->root->latest_file; f; f = f->next) {
    ++n;
    if (ctx->since.is_timestamp && f->otime.timestamp < ctx->since.timestamp) {
      break;
    }
    if (!ctx->since.is_timestamp &&
        f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      result = false;
      goto done;
    }
  }

done:
  *num_walked = n;
  return result;
}

static void update_subscription_ticks(struct watchman_client_subscription *sub,
    w_query_res *res) {
  // create a new spec that will be used the next time
  if (sub->query->since_spec) {
    w_clockspec_free(sub->query->since_spec);
  }
  sub->query->since_spec = w_clockspec_new_clock(res->root_number, res->ticks);
}

static json_t *build_subscription_results(
    struct watchman_client_subscription *sub,
    struct write_locked_watchman_root *lock)
{
  w_query_res res;
  json_t *response;
  json_t *file_list;
  char clockbuf[128];
  struct w_clockspec *since_spec = sub->query->since_spec;

  if (since_spec && since_spec->tag == w_cs_clock) {
    w_log(W_LOG_DBG, "running subscription %s rules since %" PRIu32 "\n",
        sub->name->buf, since_spec->clock.ticks);
  } else {
    w_log(W_LOG_DBG, "running subscription %s rules (no since)\n",
        sub->name->buf);
  }

  // Subscriptions never need to sync explicitly; we are only dispatched
  // at settle points which are by definition sync'd to the present time
  sub->query->sync_timeout = 0;
  // We're called by the io thread, so there's little chance that the root
  // could be legitimately blocked by something else.  That means that we
  // can use a short lock_timeout
  sub->query->lock_timeout =
      (uint32_t)cfg_get_int(lock->root, "subscription_lock_timeout_ms", 100);
  if (!w_query_execute_locked(sub->query, lock, &res, subscription_generator,
                              sub)) {
    w_log(W_LOG_ERR, "error running subscription %s query: %s",
        sub->name->buf, res.errmsg);
    w_query_result_free(&res);
    return NULL;
  }

  w_log(W_LOG_DBG, "subscription %s generated %" PRIu32 " results\n",
      sub->name->buf, res.num_results);

  if (res.num_results == 0) {
    update_subscription_ticks(sub, &res);
    w_query_result_free(&res);
    return NULL;
  }

  file_list = w_query_results_to_json(&sub->field_list,
      res.num_results, res.results);
  w_query_result_free(&res);

  response = make_response();

  // It is way too much of a hassle to try to recreate the clock value if it's
  // not a relative clock spec, and it's only going to happen on the first run
  // anyway, so just skip doing that entirely.
  if (since_spec && since_spec->tag == w_cs_clock &&
      clock_id_string(since_spec->clock.root_number, since_spec->clock.ticks,
                      clockbuf, sizeof(clockbuf))) {
    set_unicode_prop(response, "since", clockbuf);
  }
  if (clock_id_string(res.root_number, res.ticks, clockbuf, sizeof(clockbuf))) {
    set_unicode_prop(response, "clock", clockbuf);
  }
  update_subscription_ticks(sub, &res);

  set_prop(response, "is_fresh_instance", json_boolean(res.is_fresh_instance));
  set_prop(response, "files", file_list);
  set_prop(response, "root", w_string_to_json(lock->root->root_path));
  set_prop(response, "subscription", w_string_to_json(sub->name));
  set_prop(response, "unilateral", json_true());

  return response;
}

/* must be called with root and client locked */
void w_run_subscription_rules(
    struct watchman_user_client *client,
    struct watchman_client_subscription *sub,
    struct write_locked_watchman_root *lock)
{
  json_t *response = build_subscription_results(sub, lock);

  if (!response) {
    return;
  }

  add_root_warnings_to_response(response, lock->root);

  if (!enqueue_response(&client->client, response, true)) {
    w_log(W_LOG_DBG, "failed to queue sub response\n");
    json_decref(response);
  }
}

void w_cancel_subscriptions_for_root(w_root_t *root) {
  w_ht_iter_t iter;
  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) {
    do {
      struct watchman_user_client *client = w_ht_val_ptr(iter.value);
      w_ht_iter_t citer;

      if (w_ht_first(client->subscriptions, &citer)) {
        do {
          struct watchman_client_subscription *sub = w_ht_val_ptr(citer.value);

          if (sub->root == root) {
            json_t *response = make_response();

            w_log(W_LOG_ERR,
                  "Cancel subscription %.*s for client:stm=%p due to "
                  "root cancellation\n",
                  sub->name->len, sub->name->buf, client->client.stm);

            set_prop(response, "root", w_string_to_json(root->root_path));
            set_prop(response, "subscription", w_string_to_json(sub->name));
            set_prop(response, "unilateral", json_true());
            set_prop(response, "canceled", json_true());

            if (!enqueue_response(&client->client, response, true)) {
              w_log(W_LOG_DBG, "failed to queue sub cancellation\n");
              json_decref(response);
            }

            w_ht_iter_del(client->subscriptions, &citer);
          }
        } while (w_ht_next(client->subscriptions, &citer));
      }
    } while (w_ht_next(clients, &iter));
  }
  pthread_mutex_unlock(&w_client_lock);
}

/* unsubscribe /root subname
 * Cancels a subscription */
static void cmd_unsubscribe(struct watchman_client *clientbase, json_t *args)
{
  const char *name;
  w_string_t *sname;
  bool deleted;
  json_t *resp;
  const json_t *jstr;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct unlocked_watchman_root unlocked;

  if (!resolve_root_or_err(&client->client, args, 1, false, &unlocked)) {
    return;
  }

  jstr = json_array_get(args, 2);
  name = json_string_value(jstr);
  if (!name) {
    send_error_response(&client->client,
        "expected 2nd parameter to be subscription name");
    w_root_delref(unlocked.root);
    return;
  }

  sname = json_to_w_string_incref(jstr);

  pthread_mutex_lock(&w_client_lock);
  deleted = w_ht_del(client->subscriptions, w_ht_ptr_val(sname));
  pthread_mutex_unlock(&w_client_lock);

  w_string_delref(sname);

  resp = make_response();
  set_bytestring_prop(resp, "unsubscribe", name);
  set_prop(resp, "deleted", json_boolean(deleted));

  send_and_dispose_response(&client->client, resp);
  w_root_delref(unlocked.root);
}
W_CMD_REG("unsubscribe", cmd_unsubscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* subscribe /root subname {query}
 * Subscribes the client connection to the specified root. */
static void cmd_subscribe(struct watchman_client *clientbase, json_t *args)
{
  struct watchman_client_subscription *sub;
  json_t *resp;
  json_t *jfield_list;
  json_t *jname;
  w_query *query;
  json_t *query_spec;
  struct w_query_field_list field_list;
  char *errmsg;
  int defer = true; /* can't use bool because json_unpack requires int */
  json_t *defer_list = NULL;
  json_t *drop_list = NULL;
  struct watchman_user_client *client =
      (struct watchman_user_client *)clientbase;
  struct unlocked_watchman_root unlocked;
  struct write_locked_watchman_root lock;

  if (json_array_size(args) != 4) {
    send_error_response(&client->client,
                        "wrong number of arguments for subscribe");
    return;
  }

  if (!resolve_root_or_err(&client->client, args, 1, true, &unlocked)) {
    return;
  }

  jname = json_array_get(args, 2);
  if (!json_is_string(jname)) {
    send_error_response(&client->client,
        "expected 2nd parameter to be subscription name");
    goto done;
  }

  query_spec = json_array_get(args, 3);

  jfield_list = json_object_get(query_spec, "fields");
  if (!parse_field_list(jfield_list, &field_list, &errmsg)) {
    send_error_response(&client->client, "invalid field list: %s", errmsg);
    free(errmsg);
    goto done;
  }

  query = w_query_parse(unlocked.root, query_spec, &errmsg);
  if (!query) {
    send_error_response(&client->client, "failed to parse query: %s", errmsg);
    free(errmsg);
    goto done;
  }

  json_unpack(query_spec, "{s?:o}", "defer", &defer_list);
  if (defer_list && !json_is_array(defer_list)) {
    send_error_response(&client->client,
                        "defer field must be an array of strings");
    goto done;
  }
  json_unpack(query_spec, "{s?:o}", "drop", &drop_list);
  if (drop_list && !json_is_array(drop_list)) {
    send_error_response(&client->client,
                        "drop field must be an array of strings");
    goto done;
  }

  sub = calloc(1, sizeof(*sub));
  if (!sub) {
    send_error_response(&client->client, "no memory!");
    goto done;
  }

  sub->name = json_to_w_string_incref(jname);
  sub->query = query;

  json_unpack(query_spec, "{s?:b}", "defer_vcs", &defer);
  sub->vcs_defer = defer;

  if (drop_list || defer_list) {
    size_t i;

    sub->drop_or_defer = w_ht_new(2, &w_ht_string_funcs);
    if (defer_list) {
      for (i = 0; i < json_array_size(defer_list); i++) {
        w_ht_replace(sub->drop_or_defer,
            w_ht_ptr_val(json_to_w_string_incref(
            json_array_get(defer_list, i))), false);
      }
    }
    if (drop_list) {
      for (i = 0; i < json_array_size(drop_list); i++) {
        w_ht_replace(sub->drop_or_defer,
            w_ht_ptr_val(json_to_w_string_incref(json_array_get(drop_list, i))),
            true);
      }
    }
  }

  memcpy(&sub->field_list, &field_list, sizeof(field_list));
  sub->root = unlocked.root;

  pthread_mutex_lock(&w_client_lock);
  w_ht_replace(client->subscriptions, w_ht_ptr_val(sub->name),
      w_ht_ptr_val(sub));
  pthread_mutex_unlock(&w_client_lock);

  resp = make_response();
  annotate_with_clock(unlocked.root, resp);
  json_incref(jname);
  set_prop(resp, "subscribe", jname);
  add_root_warnings_to_response(resp, unlocked.root);
  send_and_dispose_response(&client->client, resp);

  w_root_lock(&unlocked, "initial subscription query", &lock);
  resp = build_subscription_results(sub, &lock);
  w_root_unlock(&lock, &unlocked);

  if (resp) {
    send_and_dispose_response(&client->client, resp);
  }
done:
  w_root_delref(unlocked.root);
}
W_CMD_REG("subscribe", cmd_subscribe, CMD_DAEMON | CMD_ALLOW_ANY_USER,
          w_cmd_realpath_root)

/* vim:ts=2:sw=2:et:
 */
