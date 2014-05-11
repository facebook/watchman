/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static w_ht_t *command_funcs = NULL;

static int compare_def(const void *A, const void *B)
{
  struct watchman_command_handler_def *a =
    *(struct watchman_command_handler_def**)A;
  struct watchman_command_handler_def *b =
    *(struct watchman_command_handler_def**)B;

  return strcmp(a->name, b->name);
}

void print_command_list_for_help(FILE *where)
{
  uint32_t i = 0, n = w_ht_size(command_funcs);
  struct watchman_command_handler_def **defs;
  w_ht_iter_t iter;

  defs = calloc(n, sizeof(*defs));
  if (w_ht_first(command_funcs, &iter)) do {
    defs[i++] = w_ht_val_ptr(iter.value);
  } while (w_ht_next(command_funcs, &iter));

  qsort(defs, n, sizeof(*defs), compare_def);

  fprintf(where, "\n\nAvailable commands:\n\n");
  for (i = 0; i < n; i++) {
    fprintf(where, "      %s\n", defs[i]->name);
  }
}

void w_register_command(struct watchman_command_handler_def *defs)
{
  if (!command_funcs) {
    command_funcs = w_ht_new(16, &w_ht_string_funcs);
  }
  w_ht_set(command_funcs,
      w_ht_ptr_val(w_string_new(defs->name)),
      w_ht_ptr_val(defs));
}

bool dispatch_command(struct watchman_client *client, json_t *args, int mode)
{
  struct watchman_command_handler_def *def;
  const char *cmd_name;
  w_string_t *cmd;

  if (!json_array_size(args)) {
    send_error_response(client,
        "invalid command (expected an array with some elements!)");
    return false;
  }

  cmd_name = json_string_value(json_array_get(args, 0));
  if (!cmd_name) {
    send_error_response(client,
        "invalid command: expected element 0 to be the command name");
    return false;
  }
  cmd = w_string_new(cmd_name);
  def = w_ht_val_ptr(w_ht_get(command_funcs, w_ht_ptr_val(cmd)));
  w_string_delref(cmd);

  if (def) {
    if ((def->flags & mode) == 0) {
      send_error_response(client,
          "command %s not available in this mode", cmd_name);
      return false;
    }
    def->func(client, args);
    return true;
  }
  send_error_response(client, "unknown command %s", cmd_name);

  return false;
}

/* vim:ts=2:sw=2:et:
 */
