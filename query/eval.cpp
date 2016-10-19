/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Query evaluator */

static w_string_t *compute_parent_path(struct w_query_ctx *ctx,
                                       struct watchman_file *file) {
  if (ctx->last_parent == file->parent) {
    return ctx->last_parent_path;
  }

  if (ctx->last_parent_path) {
    w_string_delref(ctx->last_parent_path);
  }

  ctx->last_parent_path = w_dir_copy_full_path(file->parent);
  ctx->last_parent = file->parent;

  return ctx->last_parent_path;
}

w_string_t *w_query_ctx_get_wholename(
    struct w_query_ctx *ctx
)
{
  uint32_t name_start;

  if (ctx->wholename) {
    return ctx->wholename;
  }

  if (ctx->query->relative_root != NULL) {
    // At this point every path should start with the relative root, so this is
    // legal
    name_start = ctx->query->relative_root->len + 1;
  } else {
    name_start = ctx->lock->root->root_path.size() + 1;
  }

  auto full_name = w_string::pathCat(
      {compute_parent_path(ctx, ctx->file), w_file_get_name(ctx->file)});

  // Record the name relative to the root
  ctx->wholename = full_name.slice(name_start, full_name.size() - name_start);

  return ctx->wholename;
}

bool w_query_process_file(
    w_query *query,
    struct w_query_ctx *ctx,
    struct watchman_file *file)
{
  ctx->wholename.reset();
  ctx->file = file;

  // For fresh instances, only return files that currently exist.
  if (!ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance &&
      !file->exists) {
    return true;
  }

  // We produce an output for this file if there is no expression,
  // or if the expression matched.
  if (query->expr && !query->expr->evaluate(ctx, file)) {
    // No matched
    return true;
  }

  if (ctx->dedup) {
    w_string_t *name = w_query_ctx_get_wholename(ctx);

    if (w_ht_get(ctx->dedup, w_ht_ptr_val(name))) {
      // Already present in the results, no need to emit it again
      ctx->num_deduped++;
      return true;
    }

    w_ht_set(ctx->dedup, w_ht_ptr_val(name), 1);
  }

  bool is_new;
  if (ctx->since.is_timestamp) {
    is_new = ctx->since.timestamp > file->ctime.timestamp;
  } else if (ctx->since.clock.is_fresh_instance) {
    is_new = true;
  } else {
    is_new = file->ctime.ticks > ctx->since.clock.ticks;
  }
  ctx->results.emplace_back(
      ctx->lock->root->inner.number,
      w_query_ctx_get_wholename(ctx),
      is_new,
      file);

  return true;
}

void w_match_results_free(uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  uint32_t i;

  for (i = 0; i < num_matches; i++) {
    w_string_delref(matches[i].relname);
  }
  free(matches);
}

bool w_query_file_matches_relative_root(
    struct w_query_ctx *ctx,
    struct watchman_file *f)
{
  w_string_t *parent_path;
  bool result;

  if (ctx->query->relative_root == NULL) {
    return true;
  }

  parent_path = compute_parent_path(ctx, f);
  // "in relative root" here does not mean exactly the relative root, so compare
  // against the relative root's parent.
  result = w_string_equal(parent_path, ctx->query->relative_root) ||
           w_string_startswith(parent_path, ctx->query->relative_root_slash);

  return result;
}

bool time_generator(
    w_query* query,
    struct read_locked_watchman_root* lock,
    struct w_query_ctx* ctx,
    int64_t* num_walked) {
  return lock->root->inner.view.timeGenerator(query, ctx, num_walked);
}

static bool default_generators(
    w_query* query,
    struct read_locked_watchman_root* lock,
    struct w_query_ctx* ctx,
    int64_t* num_walked) {
  bool generated = false;
  int64_t n = 0;
  int64_t total = 0;
  bool result = true;

  // Time based query
  if (ctx->since.is_timestamp || !ctx->since.clock.is_fresh_instance) {
    n = 0;
    result = time_generator(query, lock, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  // Suffix
  if (query->suffixes) {
    n = 0;
    result = lock->root->inner.view.suffixGenerator(query, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  if (query->npaths) {
    n = 0;
    result = lock->root->inner.view.pathGenerator(query, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  if (query->glob_tree) {
    n = 0;
    result = lock->root->inner.view.globGenerator(query, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (!generated) {
    n = 0;
    result = lock->root->inner.view.allFilesGenerator(query, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
  }

done:
  *num_walked = total;
  return result;
}

w_query_result::~w_query_result() {
  free(errmsg);
}

static bool execute_common(
    struct w_query_ctx* ctx,
    w_perf_t* sample,
    w_query_res* res,
    w_query_generator generator) {
  int64_t num_walked = 0;
  bool result = true;

  if (ctx->query->dedup_results) {
    ctx->dedup = w_ht_new(64, &w_ht_string_funcs);
  }

  res->is_fresh_instance = !ctx->since.is_timestamp &&
    ctx->since.clock.is_fresh_instance;

  if (!(res->is_fresh_instance && ctx->query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }

    if (!generator(ctx->query, ctx->lock, ctx, &num_walked)) {
      res->errmsg = ctx->query->errmsg;
      ctx->query->errmsg = NULL;
      result = false;
    }
  }

  if (sample->finish()) {
    sample->add_root_meta(ctx->lock->root);
    sample->add_meta(
        "query_execute",
        json_pack(
            "{s:b, s:i, s:i, s:i, s:O}",
            "fresh_instance",
            res->is_fresh_instance,
            "num_deduped",
            ctx->num_deduped,
            "num_results",
            int64_t(ctx->results.size()),
            "num_walked",
            num_walked,
            "query",
            ctx->query->query_spec));
    sample->log();
  }

  res->results = std::move(ctx->results);

  return result;
}

w_query_ctx::w_query_ctx(w_query* q, read_locked_watchman_root* lock)
    : query(q), lock(lock) {}

w_query_ctx::~w_query_ctx() {
  if (last_parent_path) {
    w_string_delref(last_parent_path);
  }
  if (dedup) {
    w_ht_free(dedup);
  }
}

bool w_query_execute_locked(
    w_query* query,
    struct write_locked_watchman_root* lock,
    w_query_res* res,
    w_query_generator generator) {
  w_query_ctx ctx(query, w_root_read_lock_from_write(lock));

  memset(res, 0, sizeof(*res));
  w_perf_t sample("query_execute");

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

  res->root_number = lock->root->inner.number;
  res->ticks = lock->root->inner.ticks;

  // Evaluate the cursor for this root
  w_clockspec_eval(lock, query->since_spec.get(), &ctx.since);

  return execute_common(&ctx, &sample, res, generator);
}

bool w_query_execute(
    w_query* query,
    struct unlocked_watchman_root* unlocked,
    w_query_res* res,
    w_query_generator generator) {
  struct write_locked_watchman_root wlock;
  struct read_locked_watchman_root rlock;
  bool result;

  w_query_ctx ctx(query, nullptr);
  memset(res, 0, sizeof(*res));

  w_perf_t sample("query_execute");

  if (query->sync_timeout &&
      !w_root_sync_to_now(unlocked, query->sync_timeout)) {
    ignore_result(asprintf(&res->errmsg, "synchronization failed: %s\n",
        strerror(errno)));
    return false;
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

  if (query->since_spec && query->since_spec->tag == w_cs_named_cursor) {
    // We need a write lock to evaluate this cursor
    if (!w_root_lock_with_timeout(unlocked, "w_query_execute_named_cursor",
                                  query->lock_timeout, &wlock)) {
      ignore_result(asprintf(&res->errmsg,
                             "couldn't acquire root wrlock within "
                             "lock_timeout of %dms. root is "
                             "currently busy (%s)\n",
                             query->lock_timeout, unlocked->root->lock_reason));
      return false;
    }
    ctx.lock = w_root_read_lock_from_write(&wlock);
    // Evaluate the cursor for this root
    w_clockspec_eval(&wlock, query->since_spec.get(), &ctx.since);

    // Note that we proceed with the rest of query while we hold our write
    // lock.  We could potentially drop the write lock and re-acquire the
    // lock as a read lock so that other queries could proceed concurrently,
    // but that would make the overall timeout situation more complex and
    // may not be a significant win in any case.
  } else {
    if (!w_root_read_lock_with_timeout(unlocked, "w_query_execute",
                                  query->lock_timeout, &rlock)) {
      ignore_result(asprintf(&res->errmsg,
                             "couldn't acquire root rdlock within "
                             "lock_timeout of %dms. root is "
                             "currently busy (%s)\n",
                             query->lock_timeout, unlocked->root->lock_reason));
      return false;
    }
    ctx.lock = &rlock;
    // Evaluate the cursor for this root
    w_clockspec_eval_readonly(&rlock, query->since_spec.get(), &ctx.since);
  }

  res->root_number = ctx.lock->root->inner.number;
  res->ticks = ctx.lock->root->inner.ticks;

  result = execute_common(&ctx, &sample, res, generator);
  // This handles the unlock in both the read and write case, as ctx.lock
  // points to the read or write lock as appropriate, and the underlying
  // unlock operation is defined to be safe for either.
  w_root_read_unlock(ctx.lock, unlocked);
  return result;
}

/* vim:ts=2:sw=2:et:
 */
