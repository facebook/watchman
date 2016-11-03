/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// log-level "debug"
// log-level "error"
// log-level "off"
static void cmd_loglevel(struct watchman_client* client, const json_ref& args) {
  const auto& label = json_to_w_string(args.at(1));
  auto level = watchman::logLabelToLevel(label);
  auto clientRef = client->shared_from_this();
  auto notify = [clientRef]() { w_event_set(clientRef->ping); };
  auto& log = watchman::getLog();

  switch (level) {
    case watchman::OFF:
      client->debugSub.reset();
      client->errorSub.reset();
      break;
    case watchman::DBG:
      client->debugSub = log.subscribe(watchman::DBG, notify);
      client->errorSub = log.subscribe(watchman::ERR, notify);
      break;
    case watchman::ERR:
    default:
      client->debugSub.reset();
      client->errorSub = log.subscribe(watchman::ERR, notify);
  }

  auto resp = make_response();
  resp.set("log_level", json_ref(args.at(1)));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("log-level", cmd_loglevel, CMD_DAEMON, NULL)

// log "debug" "text to log"
static void cmd_log(struct watchman_client* client, const json_ref& args) {
  auto level = watchman::logLabelToLevel(json_to_w_string(args.at(1)));
  auto text = json_to_w_string(args.at(2));

  watchman::log(level, text, "\n");

  auto resp = make_response();
  resp.set("logged", json_true());
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("log", cmd_log, CMD_DAEMON, NULL)

/* vim:ts=2:sw=2:et:
 */
