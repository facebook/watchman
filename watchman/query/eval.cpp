/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman/watchman.h"

#include <folly/ScopeGuard.h>
#include "watchman/CommandRegistry.h"
#include "watchman/Errors.h"
#include "watchman/LocalFileResult.h"
#include "watchman/WatchmanConfig.h"
#include "watchman/query/QueryContext.h"
#include "watchman/saved_state/SavedStateFactory.h"
#include "watchman/saved_state/SavedStateInterface.h"
#include "watchman/scm/SCM.h"
#include "watchman/watchman_root.h"

using namespace watchman;

FileResult::~FileResult() {}

folly::Optional<DType> FileResult::dtype() {
  auto statInfo = stat();
  if (!statInfo.has_value()) {
    return folly::none;
  }
  return statInfo->dtype();
}

const w_string& w_query_ctx_get_wholename(QueryContext* ctx) {
  if (ctx->wholename) {
    return ctx->wholename;
  }

  ctx->wholename = ctx->computeWholeName(ctx->file.get());
  return ctx->wholename;
}

namespace {
std::vector<w_string> computeUnconditionalLogFilePrefixes() {
  Configuration globalConfig;
  auto names =
      globalConfig.get("unconditional_log_if_results_contain_file_prefixes");

  std::vector<w_string> result;
  if (names) {
    for (auto name : names.array()) {
      result.push_back(json_to_w_string(name));
    }
  }

  return result;
}

const std::vector<w_string>& getUnconditionalLogFilePrefixes() {
  // Meyer's singleton to hold this for the life of the process
  static const std::vector<w_string> names =
      computeUnconditionalLogFilePrefixes();
  return names;
}
} // namespace

/* Query evaluator */
void w_query_process_file(
    w_query* query,
    QueryContext* ctx,
    std::unique_ptr<FileResult> file) {
  ctx->wholename.reset();
  ctx->file = std::move(file);
  SCOPE_EXIT {
    ctx->file.reset();
  };

  // For fresh instances, only return files that currently exist
  // TODO: shift this clause to execute_common and generate
  // a wrapped query: ["allof", "exists", EXPR] and execute that
  // instead of query->expr so that the lazy evaluation logic can
  // be automatically applied and avoid fetching the exists flag
  // for every file.  See also related TODO in batchFetchNow.
  if (!ctx->disableFreshInstance && !ctx->since.is_timestamp &&
      ctx->since.clock.is_fresh_instance) {
    auto exists = ctx->file->exists();
    if (!exists.has_value()) {
      // Reconsider this one later
      ctx->addToEvalBatch(std::move(ctx->file));
      return;
    }
    if (!exists.value()) {
      return;
    }
  }

  // We produce an output for this file if there is no expression,
  // or if the expression matched.
  if (query->expr) {
    auto match = query->expr->evaluate(ctx, ctx->file.get());

    if (!match.has_value()) {
      // Reconsider this one later
      ctx->addToEvalBatch(std::move(ctx->file));
      return;
    } else if (!*match) {
      return;
    }
  }

  if (ctx->query->dedup_results) {
    auto name = w_query_ctx_get_wholename(ctx);

    auto inserted = ctx->dedup.insert(name);
    if (!inserted.second) {
      // Already present in the results, no need to emit it again
      ctx->num_deduped++;
      return;
    }
  }

  auto logPrefixes = getUnconditionalLogFilePrefixes();
  if (!logPrefixes.empty()) {
    auto name = w_query_ctx_get_wholename(ctx);
    for (auto& prefix : logPrefixes) {
      if (name.piece().startsWith(prefix)) {
        ctx->namesToLog.push_back(name);
      }
    }
  }

  ctx->maybeRender(std::move(ctx->file));
}

void time_generator(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    QueryContext* ctx) {
  root->view()->timeGenerator(query, ctx);
}

static void default_generators(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    QueryContext* ctx) {
  bool generated = false;

  // Time based query
  if (ctx->since.is_timestamp || !ctx->since.clock.is_fresh_instance) {
    time_generator(query, root, ctx);
    generated = true;
  }

  if (query->paths.has_value()) {
    root->view()->pathGenerator(query, ctx);
    generated = true;
  }

  if (query->glob_tree) {
    root->view()->globGenerator(query, ctx);
    generated = true;
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (!generated) {
    root->view()->allFilesGenerator(query, ctx);
  }
}

static void execute_common(
    QueryContext* ctx,
    PerfSample* sample,
    QueryResult* res,
    QueryGenerator generator) {
  ctx->stopWatch.reset();

  if (ctx->query->dedup_results) {
    ctx->dedup.reserve(64);
  }

  // isFreshInstance is also later set by the value in ctx after generator
  res->isFreshInstance =
      !ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance;

  if (!(res->isFreshInstance && ctx->query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }
    generator(ctx->query, ctx->root, ctx);
  }
  ctx->generationDuration = ctx->stopWatch.lap();
  ctx->state = QueryContextState::Rendering;

  // We may have some file results pending re-evaluation,
  // so make sure that we process them before we get to
  // the render phase below.
  ctx->fetchEvalBatchNow();
  while (!ctx->fetchRenderBatchNow()) {
    // Depending on the implementation of the query terms and
    // the field renderers, we may need to do a couple of fetches
    // to get all that we need, so we loop until we get them all.
  }

  ctx->renderDuration = ctx->stopWatch.lap();
  ctx->state = QueryContextState::Completed;

  // For Eden instances it is possible that when running the query it was
  // discovered that it is actually a fresh instance [e.g. mount generation
  // changes or journal truncation]; update res to match
  res->isFreshInstance |= ctx->since.clock.is_fresh_instance;

  if (sample && !ctx->namesToLog.empty()) {
    auto nameList = json_array_of_size(ctx->namesToLog.size());
    for (auto& name : ctx->namesToLog) {
      nameList.array().push_back(w_string_to_json(name));

      // Avoid listing everything!
      if (nameList.array().size() >= 12) {
        break;
      }
    }

    sample->add_meta(
        "num_special_files_in_result_set",
        json_integer(ctx->namesToLog.size()));
    sample->add_meta("special_files_in_result_set", std::move(nameList));
    sample->force_log();
  }

  if (sample && sample->finish()) {
    ctx->root->addPerfSampleMetadata(*sample);
    sample->add_meta(
        "query_execute",
        json_object(
            {{"fresh_instance", json_boolean(res->isFreshInstance)},
             {"num_deduped", json_integer(ctx->num_deduped)},
             {"num_results", json_integer(json_array_size(ctx->resultsArray))},
             {"num_walked", json_integer(ctx->getNumWalked())},
             {"query", ctx->query->query_spec}}));
    sample->log();
  }

  res->resultsArray = ctx->resultsArray;
  res->dedupedFileNames = std::move(ctx->dedup);
}

// Capability indicating support for scm-aware since queries
W_CAP_REG("scm-since")

QueryResult w_query_execute(
    w_query* query,
    const std::shared_ptr<watchman_root>& root,
    QueryGenerator generator) {
  QueryResult res;
  ClockSpec resultClock(ClockPosition{});
  bool disableFreshInstance{false};
  auto requestId = query->request_id;

  PerfSample sample("query_execute");
  if (requestId && !requestId.empty()) {
    log(DBG, "request_id = ", requestId, "\n");
    sample.add_meta("request_id", w_string_to_json(requestId));
  }

  // We want to check this before we sync, as the SCM may generate changes
  // in the filesystem when running the underlying commands to query it.
  if (query->since_spec && query->since_spec->hasScmParams()) {
    auto scm = root->view()->getSCM();

    if (!scm) {
      throw QueryExecError("This root does not support SCM-aware queries.");
    }

    // Populate transition counter at start of query. This allows us to
    // determine if SCM operations ocurred concurrent with query execution.
    res.stateTransCountAtStartOfQuery = root->stateTransCount.load();
    resultClock.scmMergeBaseWith = query->since_spec->scmMergeBaseWith;
    resultClock.scmMergeBase =
        scm->mergeBaseWith(resultClock.scmMergeBaseWith, requestId);
    // Always update the saved state storage type and key, but conditionally
    // update the saved state commit id below based on wether the mergebase has
    // changed.
    if (query->since_spec->hasSavedStateParams()) {
      resultClock.savedStateStorageType =
          query->since_spec->savedStateStorageType;
      resultClock.savedStateConfig = query->since_spec->savedStateConfig;
    }

    if (resultClock.scmMergeBase != query->since_spec->scmMergeBase) {
      // The merge base is different, so on the assumption that a lot of
      // things have changed between the prior and current state of
      // the world, we're just going to ask the SCM to tell us about
      // the changes, then we're going to feed that change list through
      // a simpler watchman query.
      auto modifiedMergebase = resultClock.scmMergeBase;
      if (query->since_spec->hasSavedStateParams()) {
        // Find the most recent saved state to the new mergebase and return
        // changed files since that saved state, if available.
        auto savedStateInterface = getInterface(
            query->since_spec->savedStateStorageType,
            query->since_spec->savedStateConfig,
            scm,
            root);
        auto savedStateResult = savedStateInterface->getMostRecentSavedState(
            resultClock.scmMergeBase);
        res.savedStateInfo = savedStateResult.savedStateInfo;
        if (savedStateResult.commitId) {
          resultClock.savedStateCommitId = savedStateResult.commitId;
          // Modify the mergebase to be the saved state mergebase so we can
          // return changed files since the saved state.
          modifiedMergebase = savedStateResult.commitId;
        } else {
          // Setting the saved state commit id to the empty string alerts the
          // client that the mergebase changed, yet no saved state was
          // available. The changed files list will be relative to the prior
          // clock as if scm-aware queries were not being used at all, to ensure
          // clients have all changed files they need.
          resultClock.savedStateCommitId = w_string();
          modifiedMergebase = nullptr;
        }
      }
      // If the modified mergebase is null then we had no saved state available
      // so we need to fall back to the normal behavior of returning all changes
      // since the prior clock, so we should not update the generator in that
      // case.
      if (modifiedMergebase) {
        disableFreshInstance = true;
        generator = [root, modifiedMergebase, requestId](
                        w_query* q,
                        const std::shared_ptr<watchman_root>& r,
                        QueryContext* c) {
          auto changedFiles =
              root->view()->getSCM()->getFilesChangedSinceMergeBaseWith(
                  modifiedMergebase, requestId);

          auto pathList = json_array_of_size(changedFiles.size());
          for (auto& f : changedFiles) {
            json_array_append_new(pathList, w_string_to_json(f));
          }

          auto spec = r->view()->getMostRecentRootNumberAndTickValue();
          w_clock_t clock{0, 0};
          clock.ticks = spec.ticks;
          time(&clock.timestamp);
          for (auto& pathEntry : pathList.array()) {
            auto path = json_to_w_string(pathEntry);

            auto fullPath = w_string::pathCat({r->root_path, path});
            if (!c->fileMatchesRelativeRoot(fullPath)) {
              continue;
            }
            // Note well!  At the time of writing the LocalFileResult class
            // assumes that removed entries must have been regular files.
            // We don't have enough information returned from
            // getFilesChangedSinceMergeBaseWith() to distinguish between
            // deleted files and deleted symlinks.  Also, it is not possible
            // to see a directory returned from that call; we're only going
            // to enumerate !dirs for this case.
            w_query_process_file(
                q, c, std::make_unique<LocalFileResult>(r, fullPath, clock));
          }
        };
      } else if (query->fail_if_no_saved_state) {
        throw QueryExecError(
            "The merge base changed but no corresponding saved state was "
            "found for the new merge base. fail_if_no_saved_state was set "
            "in the query so treating this as an error");
      }
    } else {
      if (query->since_spec->hasSavedStateParams()) {
        // If the mergebase has not changed, then preserve the input value for
        // the saved state commit id so it will be accurate in subscriptions.
        resultClock.savedStateCommitId = query->since_spec->savedStateCommitId;
      }
    }
  }
  // We should skip asking SCM for the changed files if the query
  // indicated to omit those. To do so, lets just make an empty
  // generator.
  if (query->omit_changed_files) {
    generator =
        [](w_query*, const std::shared_ptr<watchman_root>&, QueryContext*) {};
  }
  QueryContext ctx{query, root, disableFreshInstance};

  // Track the query against the root.
  // This is to enable the `watchman debug-status` diagnostic command.
  // It promises only to read the read-only fields in ctx and ctx.query.
  root->queries.wlock()->insert(&ctx);
  SCOPE_EXIT {
    root->queries.wlock()->erase(&ctx);
  };
  if (query->sync_timeout.count()) {
    ctx.state = QueryContextState::WaitingForCookieSync;
    ctx.stopWatch.reset();
    try {
      root->syncToNow(query->sync_timeout, res.debugInfo.cookieFileNames);
    } catch (const std::exception& exc) {
      throw QueryExecError("synchronization failed: ", exc.what());
    }
    ctx.cookieSyncDuration = ctx.stopWatch.lap();
  }

  /* The first stage of execution is generation.
   * We generate a series of file inputs to pass to
   * the query executor.
   *
   * We evaluate each of the generators one after the
   * other.  If multiple generators are used, it is
   * possible and expected that the same file name
   * will be evaluated multiple times if those generators
   * both emit the same file.
   */

  ctx.clockAtStartOfQuery =
      ClockSpec(root->view()->getMostRecentRootNumberAndTickValue());
  ctx.lastAgeOutTickValueAtStartOfQuery =
      root->view()->getLastAgeOutTickValue();

  // Copy in any scm parameters
  res.clockAtStartOfQuery = resultClock;
  // then update the clock position portion
  res.clockAtStartOfQuery.clock = ctx.clockAtStartOfQuery.clock;

  // Evaluate the cursor for this root
  ctx.since = query->since_spec ? query->since_spec->evaluate(
                                      ctx.clockAtStartOfQuery.position(),
                                      ctx.lastAgeOutTickValueAtStartOfQuery,
                                      &root->inner.cursors)
                                : w_query_since();

  if (query->bench_iterations > 0) {
    for (uint32_t i = 0; i < query->bench_iterations; ++i) {
      QueryContext c{query, root, ctx.disableFreshInstance};
      QueryResult r;
      c.clockAtStartOfQuery = ctx.clockAtStartOfQuery;
      c.since = ctx.since;
      execute_common(&c, nullptr, &r, generator);
    }
  }

  execute_common(&ctx, &sample, &res, generator);
  return res;
}

/* vim:ts=2:sw=2:et:
 */
