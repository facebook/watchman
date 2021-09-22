/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "watchman/Clock.h"
#include "watchman/FileSystem.h"
#include "watchman/query/FileResult.h"
#include "watchman/query/QueryExpr.h"

namespace watchman {
struct GlobTree;
struct Query;
struct QueryContext;
struct QueryFieldRenderer;
using QueryFieldList = std::vector<QueryFieldRenderer*>;
class Root;
} // namespace watchman

struct watchman_file;
using watchman_root = watchman::Root;

std::shared_ptr<watchman::Query> w_query_parse(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& query);

// parse the old style since and find queries
std::shared_ptr<watchman::Query> w_query_parse_legacy(
    const std::shared_ptr<watchman_root>& root,
    const json_ref& args,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p);
void w_query_legacy_field_list(watchman::QueryFieldList* flist);

void parse_field_list(json_ref field_list, watchman::QueryFieldList* selected);
json_ref field_list_to_json_name_array(
    const watchman::QueryFieldList& fieldList);

void parse_suffixes(watchman::Query* res, const json_ref& query);
void parse_globs(watchman::Query* res, const json_ref& query);

/* vim:ts=2:sw=2:et:
 */
