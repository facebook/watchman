/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "watchman_scopeguard.h"
using watchman::Result;
using watchman::Future;
using watchman::collectAll;

FileResult::~FileResult() {}

/* Query evaluator */

const w_string& w_query_ctx_get_wholename(struct w_query_ctx* ctx) {
  uint32_t name_start;

  if (ctx->wholename) {
    return ctx->wholename;
  }

  if (ctx->query->relative_root) {
    // At this point every path should start with the relative root, so this is
    // legal
    name_start = ctx->query->relative_root.size() + 1;
  } else {
    name_start = ctx->root->root_path.size() + 1;
  }

  // Record the name relative to the root
  auto parent = ctx->file->dirName();
  if (name_start > parent.size()) {
    ctx->wholename = ctx->file->baseName().asWString();
  } else {
    parent.advance(name_start);
    ctx->wholename = w_string::build(parent, "/", ctx->file->baseName());
  }

  return ctx->wholename;
}

void w_query_process_file(
    w_query* query,
    struct w_query_ctx* ctx,
    std::unique_ptr<FileResult> file) {
  ctx->wholename.reset();
  ctx->file = std::move(file);
  SCOPE_EXIT {
    ctx->file.reset();
  };

  // For fresh instances, only return files that currently exist.
  if (!ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance &&
      !ctx->file->exists()) {
    return;
  }

  // We produce an output for this file if there is no expression,
  // or if the expression matched.
  if (query->expr && !query->expr->evaluate(ctx, ctx->file.get())) {
    // Not matched
    return;
  }

  if (ctx->query->dedup_results) {
    w_string_t *name = w_query_ctx_get_wholename(ctx);

    auto inserted = ctx->dedup.insert(name);
    if (!inserted.second) {
      // Already present in the results, no need to emit it again
      ctx->num_deduped++;
      return;
    }
  }

  bool is_new;
  if (ctx->since.is_timestamp) {
    is_new = ctx->since.timestamp > ctx->file->ctime().timestamp;
  } else if (ctx->since.clock.is_fresh_instance) {
    is_new = true;
  } else {
    is_new = ctx->file->ctime().ticks > ctx->since.clock.ticks;
  }

  auto wholename = w_query_ctx_get_wholename(ctx);
  watchman_rule_match match(
      ctx->clockAtStartOfQuery.rootNumber,
      wholename,
      is_new,
      std::move(ctx->file));

  if (ctx->query->renderUsesFutures) {
    // Conceptually all we need to do here is append the future to
    // resultsToRender and then collectAll at the end of the query.  That
    // requires O(num-matches x num-fields) memory usage of the future related
    // data for the duration of the query.  In order to keep things down to a
    // more reasonable size, if the future is immediately ready we can append to
    // the results directly, and we can also speculatively do the same for any
    // pending items that happen to complete in between matches.  That makes
    // this code look a little more complex, but it is worth it for very large
    // result sets.
    auto future =
        file_result_to_json_future(ctx->query->fieldList, std::move(match));
    if (future.isReady()) {
      json_array_append_new(ctx->resultsArray, std::move(future.get()));
    } else {
      ctx->resultsToRender.emplace_back(std::move(future));
    }
    ctx->speculativeRenderCompletion();
  } else {
    json_array_append_new(
        ctx->resultsArray, file_result_to_json(ctx->query->fieldList, match));
  }
}

void w_query_ctx::speculativeRenderCompletion() {
  while (!resultsToRender.empty() && resultsToRender.front().isReady()) {
    json_array_append_new(
        resultsArray, std::move(resultsToRender.front().get()));
    resultsToRender.pop_front();
  }
}

bool w_query_file_matches_relative_root(
    struct w_query_ctx* ctx,
    const watchman_file* f) {
  bool result;

  if (!ctx->query->relative_root) {
    return true;
  }

  auto parent_path = f->parent->getFullPath();
  // "in relative root" here does not mean exactly the relative root, so compare
  // against the relative root's parent.
  result = w_string_equal(parent_path, ctx->query->relative_root) ||
           w_string_startswith(parent_path, ctx->query->relative_root_slash);

  return result;
}

void time_generator(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    struct w_query_ctx* ctx) {
  root->inner.view->timeGenerator(query, ctx);
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
    root->inner.view->suffixGenerator(query, ctx);
    generated = true;
  }

  if (!query->paths.empty()) {
    root->inner.view->pathGenerator(query, ctx);
    generated = true;
  }

  if (query->glob_tree) {
    root->inner.view->globGenerator(query, ctx);
    generated = true;
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (!generated) {
    root->inner.view->allFilesGenerator(query, ctx);
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

  res->is_fresh_instance = !ctx->since.is_timestamp &&
    ctx->since.clock.is_fresh_instance;

  if (!(res->is_fresh_instance && ctx->query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }

    generator(ctx->query, ctx->root, ctx);
  }

  if (!ctx->resultsToRender.empty()) {
    collectAll(ctx->resultsToRender.begin(), ctx->resultsToRender.end())
        .then([&](Result<std::vector<Result<json_ref>>>&& results) {
          auto& vec = results.value();
          for (auto& item : vec) {
            json_array_append_new(ctx->resultsArray, std::move(item.value()));
          }
        })
        .get();
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

w_query_ctx::w_query_ctx(w_query* q, const std::shared_ptr<w_root_t>& root)
    : query(q), root(root), resultsArray(json_array()) {
  // build a template for the serializer
  if (query->fieldList.size() > 1) {
    json_array_set_template_new(
        resultsArray, field_list_to_json_name_array(query->fieldList));
  }
}

w_query_res w_query_execute(
    w_query* query,
    const std::shared_ptr<w_root_t>& root,
    w_query_generator generator) {
  w_query_res res;
  w_query_ctx ctx(query, nullptr);

  w_perf_t sample("query_execute");

  if (query->sync_timeout.count() && !root->syncToNow(query->sync_timeout)) {
    throw QueryExecError("synchronization failed: ", strerror(errno));
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

  ctx.root = root;

  ctx.clockAtStartOfQuery =
      root->inner.view->getMostRecentRootNumberAndTickValue();
  ctx.lastAgeOutTickValueAtStartOfQuery =
      root->inner.view->getLastAgeOutTickValue();
  res.clockAtStartOfQuery = ctx.clockAtStartOfQuery;

  // Evaluate the cursor for this root
  ctx.since = query->since_spec
      ? query->since_spec->evaluate(
            ctx.clockAtStartOfQuery,
            ctx.lastAgeOutTickValueAtStartOfQuery,
            &root->inner.cursors)
      : w_query_since();

  if (query->query_spec.get_default("bench")) {
    for (auto i = 0; i < 100; ++i) {
      w_query_ctx c(query, root);
      w_query_res r;
      c.clockAtStartOfQuery = ctx.clockAtStartOfQuery;
      execute_common(&c, nullptr, &r, generator);
    }
  }

  execute_common(&ctx, &sample, &res, generator);
  return res;
}

/* vim:ts=2:sw=2:et:
 */
