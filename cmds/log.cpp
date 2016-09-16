/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static int parse_log_level(const char *str)
{
  if (!strcmp(str, "debug")) {
    return W_LOG_DBG;
  } else if (!strcmp(str, "error")) {
    return W_LOG_ERR;
  } else if (!strcmp(str, "off")) {
    return W_LOG_OFF;
  }
  return -1;
}

// log-level "debug"
// log-level "error"
// log-level "off"
static void cmd_loglevel(struct watchman_client *client, json_t *args)
{
  const char *cmd, *str;
  json_t *resp;
  int level;

  if (json_unpack(args, "[us]", &cmd, &str)) {
    send_error_response(client, "expected a debug level argument");
    return;
  }

  level = parse_log_level(str);
  if (level == -1) {
    send_error_response(client, "invalid debug level %s", str);
    return;
  }

  pthread_mutex_lock(&w_client_lock);
  client->log_level = level;
  pthread_mutex_unlock(&w_client_lock);

  resp = make_response();
  set_unicode_prop(resp, "log_level", str);

  send_and_dispose_response(client, resp);
}
W_CMD_REG("log-level", cmd_loglevel, CMD_DAEMON, NULL)

// log "debug" "text to log"
static void cmd_log(struct watchman_client *client, json_t *args)
{
  const char *cmd, *str, *text;
  json_t *resp;
  int level;

  if (json_unpack(args, "[uss]", &cmd, &str, &text)) {
    send_error_response(client, "expected a string to log");
    return;
  }

  level = parse_log_level(str);
  if (level == -1) {
    send_error_response(client, "invalid debug level %s", str);
    return;
  }

  w_log(level, "%s\n", text);

  resp = make_response();
  set_prop(resp, "logged", json_true());
  send_and_dispose_response(client, resp);
}
W_CMD_REG("log", cmd_log, CMD_DAEMON, NULL)

/* vim:ts=2:sw=2:et:
 */
