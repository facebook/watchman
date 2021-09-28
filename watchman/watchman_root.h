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

std::shared_ptr<watchman::Root> w_root_resolve(
    const char* path,
    bool auto_watch);

std::shared_ptr<watchman::Root> w_root_resolve_for_client_mode(
    const char* filename);

std::shared_ptr<watchman::Root>
root_resolve(const char* filename, bool auto_watch, bool* created);
