/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* find /root [patterns] */
void cmd_find(struct watchman_client *client, json_t *args)
{
  struct watchman_rule *rules = NULL;
  w_root_t *root;
  char buf[128];

  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(2, args, &rules, NULL, buf, sizeof(buf))) {
    send_error_response(client, "invalid rule spec: %s", buf);
    w_root_delref(root);
    return;
  }

  /* now find all matching files */
  run_rules(client, root, NULL, rules);
  w_free_rules(rules);
  w_root_delref(root);
}


/* vim:ts=2:sw=2:et:
 */

