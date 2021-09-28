/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/Synchronized.h>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include "watchman/CookieSync.h"
#include "watchman/FileSystem.h"
#include "watchman/PendingCollection.h"
#include "watchman/PubSub.h"
#include "watchman/QueryableView.h"
#include "watchman/TriggerCommand.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/root/Root.h"
#include "watchman/watchman_ignore.h"

#define HINT_NUM_DIRS 128 * 1024
#define CFG_HINT_NUM_DIRS "hint_num_dirs"

#define DEFAULT_SETTLE_PERIOD 20
constexpr std::chrono::milliseconds DEFAULT_QUERY_SYNC_MS(60000);

/* Idle out watches that haven't had activity in several days */
#define DEFAULT_REAP_AGE (86400 * 5)

std::shared_ptr<watchman::Root> w_root_resolve(
    const char* path,
    bool auto_watch);

std::shared_ptr<watchman::Root> w_root_resolve_for_client_mode(
    const char* filename);

bool did_file_change(
    const watchman::FileInformation* saved,
    const watchman::FileInformation* fresh);
std::shared_ptr<watchman::Root>
root_resolve(const char* filename, bool auto_watch, bool* created);

void handle_open_errno(
    watchman::Root& root,
    struct watchman_dir* dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err);
