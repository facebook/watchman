/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// Functions relating to the per-user service

static void cmd_shutdown(struct watchman_client *client, json_t *args) {
  json_t *resp = make_response();
  unused_parameter(args);

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");
  w_request_shutdown();

  set_prop(resp, "shutdown-server", json_true());
  send_and_dispose_response(client, resp);
}
W_CMD_REG("shutdown-server", cmd_shutdown, CMD_DAEMON|CMD_POISON_IMMUNE, NULL)

void add_root_warnings_to_response(json_t *response, w_root_t *root) {
  char *str = NULL;
  char *full = NULL;

  if (!root->last_recrawl_reason && !root->warning) {
    return;
  }

  if (root->last_recrawl_reason) {
    ignore_result(
        asprintf(&str, "Recrawled this watch %d times, most recently because:\n"
                       "%.*s\n"
                       "To resolve, please review the information on\n"
                       "%s#recrawl",
                 root->recrawl_count, root->last_recrawl_reason->len,
                 root->last_recrawl_reason->buf, cfg_get_trouble_url()));
  }

  ignore_result(asprintf(
      &full,
      "%.*s%s" // root->warning
      "%s\n"   // str (last recrawl reason)
      "To clear this warning, run:\n"
      "`watchman watch-del %.*s ; watchman watch-project %.*s`\n",
      root->warning ? root->warning->len : 0,
      root->warning ? root->warning->buf : "",
      root->warning && str ? "\n" : "", // newline if we have both strings
      str ? str : "", root->root_path->len, root->root_path->buf,
      root->root_path->len, root->root_path->buf));

  if (full) {
    set_prop(response, "warning", json_string_nocheck(full));
  }
  free(str);
  free(full);
}

w_root_t *resolve_root_or_err(struct watchman_client *client, json_t *args,
                              int root_index, bool create) {
  w_root_t *root;
  const char *root_name;
  char *errmsg = NULL;
  json_t *ele;

  ele = json_array_get(args, root_index);
  if (!ele) {
    send_error_response(client, "wrong number of arguments");
    return NULL;
  }

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(client, "invalid value for argument %d, expected "
                                "a string naming the root dir",
                        root_index);
    return NULL;
  }

  if (client->client_mode) {
    root = w_root_resolve_for_client_mode(root_name, &errmsg);
  } else {
    root = w_root_resolve(root_name, create, &errmsg);
  }

  if (!root) {
    send_error_response(client, "unable to resolve root %s: %s", root_name,
                        errmsg);
    free(errmsg);
  }
  return root;
}

/* vim:ts=2:sw=2:et:
 */
