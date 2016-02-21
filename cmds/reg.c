/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static w_ht_t *command_funcs = NULL;
static w_ht_t *capabilities = NULL;

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
  if (!defs) {
    abort();
  }
  if (w_ht_first(command_funcs, &iter)) do {
    defs[i++] = w_ht_val_ptr(iter.value);
  } while (w_ht_next(command_funcs, &iter));

  if (n > 0) {
    qsort(defs, n, sizeof(*defs), compare_def);
  }

  fprintf(where, "\n\nAvailable commands:\n\n");
  for (i = 0; i < n; i++) {
    fprintf(where, "      %s\n", defs[i]->name);
  }
}

void w_register_command(struct watchman_command_handler_def *defs)
{
  char capname[128];

  if (!command_funcs) {
    command_funcs = w_ht_new(16, &w_ht_string_funcs);
  }
  w_ht_set(command_funcs,
      w_ht_ptr_val(w_string_new(defs->name)),
      w_ht_ptr_val(defs));

  snprintf(capname, sizeof(capname), "cmd-%s", defs->name);
  w_capability_register(capname);
}

static struct watchman_command_handler_def *lookup(
    json_t *args, char **errmsg, int mode)
{
  struct watchman_command_handler_def *def;
  const char *cmd_name;
  w_string_t *cmd;

  if (!json_array_size(args)) {
    ignore_result(asprintf(errmsg,
        "invalid command (expected an array with some elements!)"));
    return false;
  }

  cmd_name = json_string_value(json_array_get(args, 0));
  if (!cmd_name) {
    ignore_result(asprintf(errmsg,
        "invalid command: expected element 0 to be the command name"));
    return false;
  }
  cmd = w_string_new(cmd_name);
  def = w_ht_val_ptr(w_ht_get(command_funcs, w_ht_ptr_val(cmd)));
  w_string_delref(cmd);

  if (def) {
    if (mode && ((def->flags & mode) == 0)) {
      ignore_result(asprintf(errmsg,
          "command %s not available in this mode", cmd_name));
      return NULL;
    }
    return def;
  }

  if (mode) {
    ignore_result(asprintf(errmsg, "unknown command %s", cmd_name));
  }

  return NULL;
}

void preprocess_command(json_t *args, enum w_pdu_type output_pdu)
{
  char *errmsg = NULL;
  struct watchman_command_handler_def *def;

  def = lookup(args, &errmsg, 0);

  if (!def && !errmsg) {
    // Nothing known about it, pass the command on anyway for forwards
    // compatibility
    return;
  }

  if (!errmsg && def->cli_validate) {
    def->cli_validate(args, &errmsg);
  }

  if (errmsg) {
    w_jbuffer_t jr;

    json_t *err = json_pack(
      "{s:s, s:s, s:b}",
      "error", errmsg,
      "version", PACKAGE_VERSION,
      "cli_validated", true
    );

    w_json_buffer_init(&jr);
    w_ser_write_pdu(output_pdu, &jr, w_stm_stdout(), err);
    json_decref(err);
    w_json_buffer_free(&jr);

    free(errmsg);
    exit(1);
  }
}

bool dispatch_command(struct watchman_client *client, json_t *args, int mode)
{
  struct watchman_command_handler_def *def;
  char *errmsg = NULL;

  def = lookup(args, &errmsg, mode);

  if (!def) {
    send_error_response(client, "%s", errmsg);
    free(errmsg);
    return false;
  }

  if (poisoned_reason && (def->flags & CMD_POISON_IMMUNE) == 0) {
    send_error_response(client, "%s", poisoned_reason);
    return false;
  }

  w_log(W_LOG_DBG, "dispatch_command: %s\n", def->name);
  def->func(client, args);
  w_log(W_LOG_DBG, "dispatch_command: %s (completed)\n", def->name);
  return true;
}

void w_capability_register(const char *name) {
  if (!capabilities) {
    capabilities = w_ht_new(128, &w_ht_string_funcs);
  }
  w_ht_set(capabilities,
      w_ht_ptr_val(w_string_new(name)),
      true);
}

bool w_capability_supported(const char *name) {
  bool res;
  w_string_t *namestr = w_string_new(name);
  res = w_ht_get(capabilities, w_ht_ptr_val(namestr));
  w_string_delref(namestr);
  return res;
}

json_t *w_capability_get_list(void) {
  json_t *arr = json_array_of_size(w_ht_size(capabilities));
  w_ht_iter_t iter;

  w_ht_first(capabilities, &iter);
  do {
    w_string_t *name = w_ht_val_ptr(iter.key);
    json_array_append(arr, w_string_to_json(name));
  } while (w_ht_next(capabilities, &iter));

  return arr;
}

/* vim:ts=2:sw=2:et:
 */
