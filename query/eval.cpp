/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include <folly/ScopeGuard.h>
#include "LocalFileResult.h"
#include "saved_state/SavedStateInterface.h"

using namespace watchman;

FileResult::~FileResult() {}

w_string w_query_ctx::computeWholeName(FileResult* file) const {
  uint32_t name_start;

  if (query->relative_root) {
    // At this point every path should start with the relative root, so this is
    // legal
    name_start = query->relative_root.size() + 1;
  } else {
    name_start = root->root_path.size() + 1;
  }

  // Record the name relative to the root
  auto parent = file->dirName();
  if (name_start > parent.size()) {
    return file->baseName().asWString();
  }
  parent.advance(name_start);
  return w_string::build(parent, "/", file->baseName());
}

const w_string& w_query_ctx_get_wholename(struct w_query_ctx* ctx) {
  if (ctx->wholename) {
    return ctx->wholename;
  }

  ctx->wholename = ctx->computeWholeName(ctx->file.get());
  return ctx->wholename;
}

/* Query evaluator */
void w_query_process_file(
    w_query* query,
    struct w_query_ctx* ctx,
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
    w_string_t* name = w_query_ctx_get_wholename(ctx);

    auto inserted = ctx->dedup.insert(name);
    if (!inserted.second) {
      // Already present in the results, no need to emit it again
      ctx->num_deduped++;
      return;
    }
  }

  ctx->maybeRender(std::move(ctx->file));
}

bool w_query_ctx::dirMatchesRelativeRoot(w_string_piece fullDirectoryPath) {
  if (!query->relative_root) {
    return true;
  }

  // "matches relative root" here can be either an exact match for
  // the relative root, or some path below it, so we compare against
  // both.  relative_root_slash is a precomputed version of relative_root
  // with the trailing slash to make this comparison very slightly cheaper
  // and less awkward to express in code.
  return fullDirectoryPath == query->relative_root ||
      fullDirectoryPath.startsWith(query->relative_root_slash);
}

bool w_query_ctx::fileMatchesRelativeRoot(w_string_piece fullFilePath) {
  // dirName() scans the string contents; avoid it with this cheap test
  if (!query->relative_root) {
    return true;
  }

  return dirMatchesRelativeRoot(fullFilePath.dirName());
}

bool w_query_ctx::fileMatchesRelativeRoot(const watchman_file* f) {
  // getFullPath() allocates memory; avoid it with this cheap test
  if (!query->relative_root) {
    return true;
  }

  return dirMatchesRelativeRoot(f->parent->getFullPath());
}

void time_generator(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    struct w_query_ctx* ctx) {
  root->view()->timeGenerator(query, ctx);
}

static void default_generators(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    struct w_query_ctx* ctx) {
  bool generated = false;

  // Time based query
  if (ctx->since.is_timestamp || !ctx->since.clock.is_fresh_instance) {
    time_generator(query, root, ctx);
    generated = true;
  }

  // Suffix
  if (!query->suffixes.empty()) {
    root->view()->suffixGenerator(query, ctx);
    generated = true;
  }

  if (!query->paths.empty()) {
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
    struct w_query_ctx* ctx,
    w_perf_t* sample,
    w_query_res* res,
    w_query_generator generator) {
  if (ctx->query->dedup_results) {
    ctx->dedup.reserve(64);
  }

  res->is_fresh_instance =
      !ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance;

  if (!(res->is_fresh_instance && ctx->query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }

    generator(ctx->query, ctx->root, ctx);
  }

  // We may have some file results pending re-evaluation,
  // so make sure that we process them before we get to
  // the render phase below.
  ctx->fetchEvalBatchNow();
  while (!ctx->fetchRenderBatchNow()) {
    // Depending on the implementation of the query terms and
    // the field renderers, we may need to do a couple of fetches
    // to get all that we need, so we loop until we get them all.
  }

  if (sample && sample->finish()) {
    sample->add_root_meta(ctx->root);
    sample->add_meta(
        "query_execute",
        json_object(
            {{"fresh_instance", json_boolean(res->is_fresh_instance)},
             {"num_deduped", json_integer(ctx->num_deduped)},
             {"num_results", json_integer(json_array_size(ctx->resultsArray))},
             {"num_walked", json_integer(ctx->getNumWalked())},
             {"query", ctx->query->query_spec}}));
    sample->log();
  }

  res->resultsArray = ctx->resultsArray;
  res->dedupedFileNames = std::move(ctx->dedup);
}

w_query_ctx::w_query_ctx(
    w_query* q,
    const std::shared_ptr<w_root_t>& root,
    bool disableFreshInstance)
    : query(q),
      root(root),
      resultsArray(json_array()),
      disableFreshInstance{disableFreshInstance} {
  // build a template for the serializer
  if (query->fieldList.size() > 1) {
    json_array_set_template_new(
        resultsArray, field_list_to_json_name_array(query->fieldList));
  }
}

void w_query_ctx::addToEvalBatch(std::unique_ptr<FileResult>&& file) {
  evalBatch_.emplace_back(std::move(file));

  // Find a balance between local memory usage, latency in fetching
  // and the cost of fetching the data needed to re-evaluate this batch.
  // TODO: maybe allow passing this number in via the query?
  if (evalBatch_.size() >= 20480) {
    fetchEvalBatchNow();
  }
}

void w_query_ctx::fetchEvalBatchNow() {
  if (evalBatch_.empty()) {
    return;
  }
  evalBatch_.front()->batchFetchProperties(evalBatch_);

  auto toProcess = std::move(evalBatch_);

  for (auto& file : toProcess) {
    w_query_process_file(query, this, std::move(file));
  }

  w_assert(evalBatch_.empty(), "should have no files that NeedDataLoad");
}

void w_query_ctx::maybeRender(std::unique_ptr<FileResult>&& file) {
  auto maybeRendered = file_result_to_json(query->fieldList, file, this);
  if (maybeRendered.has_value()) {
    json_array_append_new(resultsArray, std::move(maybeRendered.value()));
    return;
  }

  addToRenderBatch(std::move(file));
}

void w_query_ctx::addToRenderBatch(std::unique_ptr<FileResult>&& file) {
  renderBatch_.emplace_back(std::move(file));
  // TODO: maybe allow passing this number in via the query?
  if (renderBatch_.size() >= 1024) {
    fetchRenderBatchNow();
  }
}

bool w_query_ctx::fetchRenderBatchNow() {
  if (renderBatch_.empty()) {
    return true;
  }
  renderBatch_.front()->batchFetchProperties(renderBatch_);

  auto toProcess = std::move(renderBatch_);

  for (auto& file : toProcess) {
    auto maybeRendered = file_result_to_json(query->fieldList, file, this);
    if (maybeRendered.has_value()) {
      json_array_append_new(resultsArray, std::move(maybeRendered.value()));
    } else {
      renderBatch_.emplace_back(std::move(file));
    }
  }

  return renderBatch_.empty();
}

// Capability indicating support for scm-aware since queries
W_CAP_REG("scm-since")

w_query_res w_query_execute(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    w_query_generator generator) {
  w_query_res res;
  std::shared_ptr<w_query> altQuery;
  ClockSpec resultClock(ClockPosition{});
  bool disableFreshInstance{false};
  auto requestId = query->request_id;

  w_perf_t sample("query_execute");
  if (requestId && !requestId.empty()) {
    log(DBG, "request_id = ", requestId, "\n");
    sample.add_meta("request_id", w_string_to_json(requestId));
  }

  // We want to check this before we sync, as the SCM may generate changes
  // in the filesystem when running the underlying commands to query it.
  if (query->since_spec && query->since_spec->hasScmParams()) {
    auto scm = root->view()->getSCM();

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
        auto savedStateInterface = SavedStateInterface::getInterface(
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
                        const std::shared_ptr<w_root_t>& r,
                        struct w_query_ctx* c) {
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
      }
    } else {
      if (query->since_spec->hasSavedStateParams()) {
        // If the mergebase has not changed, then preserve the input value for
        // the saved state commit id so it will be accurate in subscriptions.
        resultClock.savedStateCommitId = query->since_spec->savedStateCommitId;
      }
    }
  }

  w_query_ctx ctx(query, root, disableFreshInstance);
  if (query->sync_timeout.count()) {
    try {
      root->syncToNow(query->sync_timeout);
    } catch (const std::exception& exc) {
      throw QueryExecError("synchronization failed: ", exc.what());
    }
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
      w_query_ctx c(query, root, ctx.disableFreshInstance);
      w_query_res r;
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
