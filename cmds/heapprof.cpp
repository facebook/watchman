/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/String.h>
#include <folly/memory/Malloc.h>
#include "watchman.h"

#if (defined(USE_JEMALLOC) || defined(FOLLY_USE_JEMALLOC)) && !FOLLY_SANITIZE

/** This command is present to manually trigger a
 * heap profile dump when jemalloc is in use.
 * Since there is a complicated relationship with our build system,
 * it is only included in the folly enabled portions of watchman.
 */
static void cmd_debug_prof_dump(
    struct watchman_client* client,
    const json_ref&) {
  if (!folly::usingJEMalloc()) {
    throw std::runtime_error("jemalloc is not in use");
  }

  auto result = mallctl("prof.dump", nullptr, nullptr, nullptr, 0);
  auto resp = make_response();
  resp.set(
      "prof.dump",
      w_string_to_json(
          watchman::to<std::string>(
              "mallctl prof.dump returned: ", folly::errnoStr(result).c_str())
              .c_str()));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-prof-dump", cmd_debug_prof_dump, CMD_DAEMON, NULL)
#endif
