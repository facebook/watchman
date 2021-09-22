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

using watchman_root = watchman::Root;

namespace watchman {

class PerfSample;

} // namespace watchman

std::shared_ptr<watchman_root> w_root_resolve(
    const char* path,
    bool auto_watch);

std::shared_ptr<watchman_root> w_root_resolve_for_client_mode(
    const char* filename);
bool findEnclosingRoot(
    const w_string& fileName,
    w_string_piece& prefix,
    w_string_piece& relativePath);

void w_root_free_watched_roots();
json_ref w_root_stop_watch_all();
void w_root_reap();

bool did_file_change(
    const watchman::FileInformation* saved,
    const watchman::FileInformation* fresh);
extern std::atomic<long> live_roots;

extern folly::Synchronized<
    std::unordered_map<w_string, std::shared_ptr<watchman_root>>>
    watched_roots;

std::shared_ptr<watchman_root>
root_resolve(const char* filename, bool auto_watch, bool* created);

void handle_open_errno(
    watchman_root& root,
    struct watchman_dir* dir,
    std::chrono::system_clock::time_point now,
    const char* syscall,
    const std::error_code& err);

bool w_root_save_state(json_ref& state);
bool w_root_load_state(const json_ref& state);
json_ref w_root_watch_list_to_json();
