/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

/* Query evaluator */

bool w_query_expr_evaluate(
    w_query_expr *expr,
    struct w_query_ctx *ctx,
    struct watchman_file *file)
{
  return expr->evaluate(ctx, file, expr->data);
}

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
  struct watchman_rule_match *m;

  ctx->wholename.reset();
  ctx->file = file;

  // For fresh instances, only return files that currently exist.
  if (!ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance &&
      !file->exists) {
    return true;
  }

  // We produce an output for this file if there is no expression,
  // or if the expression matched.
  if (query->expr && !w_query_expr_evaluate(query->expr, ctx, file)) {
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

  // Need more room?
  if (ctx->num_results + 1 > ctx->num_allocd) {
    uint32_t new_num = ctx->num_allocd ? ctx->num_allocd * 2 : 64;
    struct watchman_rule_match *res;

    res = (watchman_rule_match*)realloc(ctx->results, new_num * sizeof(*res));
    if (!res) {
      w_log(W_LOG_ERR, "out of memory while capturing matches!\n");
      return false;
    }

    ctx->results = res;
    ctx->num_allocd = new_num;
  }

  m = &ctx->results[ctx->num_results++];

  m->root_number = ctx->lock->root->inner.number;
  m->relname = w_query_ctx_get_wholename(ctx);
  if (!m->relname) {
    w_log(W_LOG_ERR, "out of memory while capturing matches!\n");
    return false;
  }
  w_string_addref(m->relname);

  m->file = file;
  if (ctx->since.is_timestamp) {
    m->is_new = ctx->since.timestamp > file->ctime.timestamp;
  } else if (ctx->since.clock.is_fresh_instance) {
    m->is_new = true;
  } else {
    m->is_new = file->ctime.ticks > ctx->since.clock.ticks;
  }

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

static bool time_generator(w_query *query,
                           struct read_locked_watchman_root *lock,
                           struct w_query_ctx *ctx, int64_t *num_walked) {
  struct watchman_file *f;
  int64_t n = 0;
  bool result = true;

  // Walk back in time until we hit the boundary
  for (f = lock->root->inner.latest_file; f; f = f->next) {
    ++n;
    if (ctx->since.is_timestamp && f->otime.timestamp < ctx->since.timestamp) {
      break;
    }
    if (!ctx->since.is_timestamp &&
        f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      result = false;
      goto done;
    }
  }

done:
  *num_walked = n;
  return result;
}

static bool suffix_generator(w_query *query,
                             struct read_locked_watchman_root *lock,
                             struct w_query_ctx *ctx, int64_t *num_walked) {
  uint32_t i;
  struct watchman_file *f;
  int64_t n = 0;
  bool result = true;

  for (i = 0; i < query->nsuffixes; i++) {
    // Head of suffix index for this suffix
    auto it = lock->root->inner.suffixes.find(query->suffixes[i]);
    if (it == lock->root->inner.suffixes.end()) {
      continue;
    }

    // Walk and process
    for (f = it->second->head; f; f = f->suffix_next) {
      ++n;
      if (!w_query_file_matches_relative_root(ctx, f)) {
        continue;
      }

      if (!w_query_process_file(query, ctx, f)) {
        result = false;
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

static bool all_files_generator(w_query *query,
                                struct read_locked_watchman_root *lock,
                                struct w_query_ctx *ctx, int64_t *num_walked) {
  struct watchman_file *f;
  int64_t n = 0;
  bool result = true;

  for (f = lock->root->inner.latest_file; f; f = f->next) {
    ++n;
    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      result = false;
      goto done;
    }
  }

done:
  *num_walked = n;
  return result;
}

static bool dir_generator(
    w_query* query,
    struct read_locked_watchman_root* lock,
    struct w_query_ctx* ctx,
    const watchman_dir* dir,
    uint32_t depth,
    int64_t* num_walked) {
  int64_t n = 0;
  bool result = true;

  for (auto& it : dir->files) {
    auto file = it.second.get();
    ++n;

    if (!w_query_process_file(query, ctx, file)) {
      result = false;
      goto done;
    }
  }

  if (depth > 0) {
    for (auto &it : dir->dirs) {
      const auto child = it.second.get();
      int64_t child_walked = 0;

      result = dir_generator(query, lock, ctx, child, depth - 1, &child_walked);
      n += child_walked;
      if (!result) {
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

static bool path_generator(
    w_query *query,
    struct read_locked_watchman_root *lock,
    struct w_query_ctx *ctx,
    int64_t *num_walked)
{
  w_string_t *relative_root;
  struct watchman_file *f;
  uint32_t i;
  int64_t n = 0;
  bool result = true;

  if (query->relative_root != NULL) {
    relative_root = query->relative_root;
  } else {
    relative_root = lock->root->root_path;
  }

  for (i = 0; i < query->npaths; i++) {
    const watchman_dir* dir;
    w_string_t *file_name;
    w_string dir_name;

    // Compose path with root
    auto full_name = w_string::pathCat({relative_root, query->paths[i].name});

    // special case of root dir itself
    if (w_string_equal(lock->root->root_path, full_name)) {
      // dirname on the root is outside the root, which is useless
      dir = w_root_resolve_dir_read(lock, full_name);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = full_name.dirName();
    if (!dir_name) {
      continue;
    }

    dir = w_root_resolve_dir_read(lock, dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      continue;
    }

    if (!dir->files.empty()) {
      file_name = w_string_basename(query->paths[i].name);
      f = dir->getChildFile(file_name);
      w_string_delref(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !S_ISDIR(f->stat.mode))) {
        ++n;
        if (!w_query_process_file(query, ctx, f)) {
          result = false;
          goto done;
        }
        continue;
      }
    }

    // Is it a dir?
    if (dir->dirs.empty()) {
      continue;
    }

    dir = dir->getChildDir(full_name.baseName());
is_dir:
    // We got a dir; process recursively to specified depth
    if (dir) {
      int64_t child_walked = 0;
      result = dir_generator(
          query, lock, ctx, dir, query->paths[i].depth, &child_walked);
      n += child_walked;
      if (!result) {
        goto done;
      }
    }
  }

done:
  *num_walked = n;
  return result;
}

static bool default_generators(w_query *query,
                               struct read_locked_watchman_root *lock,
                               struct w_query_ctx *ctx, void *gendata,
                               int64_t *num_walked) {
  bool generated = false;
  int64_t n = 0;
  int64_t total = 0;
  bool result = true;

  unused_parameter(gendata);

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
    result = suffix_generator(query, lock, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  if (query->npaths) {
    n = 0;
    result = path_generator(query, lock, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
    generated = true;
  }

  if (query->glob_tree) {
    n = 0;
    result = glob_generator(query, lock, ctx, &n);
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
    result = all_files_generator(query, lock, ctx, &n);
    total += n;
    if (!result) {
      goto done;
    }
  }

done:
  *num_walked = total;
  return result;
}

void w_query_result_free(w_query_res *res)
{
  free(res->errmsg);
  res->errmsg = NULL;
  w_match_results_free(res->num_results, res->results);
  res->results = NULL;
}

static bool execute_common(struct w_query_ctx *ctx, w_perf_t *sample,
                           w_query_res *res, w_query_generator generator,
                           void *gendata) {
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

    if (!generator(ctx->query, ctx->lock, ctx, gendata, &num_walked)) {
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
            ctx->num_results,
            "num_walked",
            num_walked,
            "query",
            ctx->query->query_spec));
    sample->log();
  }

  ctx->wholename.reset();
  if (ctx->last_parent_path) {
    w_string_delref(ctx->last_parent_path);
  }
  if (ctx->dedup) {
    w_ht_free(ctx->dedup);
  }
  res->results = ctx->results;
  res->num_results = ctx->num_results;

  return result;
}

bool w_query_execute_locked(
    w_query *query,
    struct write_locked_watchman_root *lock,
    w_query_res *res,
    w_query_generator generator,
    void *gendata)
{
  struct w_query_ctx ctx;

  memset(&ctx, 0, sizeof(ctx));
  ctx.query = query;
  ctx.lock = w_root_read_lock_from_write(lock);

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
  w_clockspec_eval(lock, query->since_spec, &ctx.since);

  return execute_common(&ctx, &sample, res, generator, gendata);
}

bool w_query_execute(
    w_query *query,
    struct unlocked_watchman_root *unlocked,
    w_query_res *res,
    w_query_generator generator,
    void *gendata)
{
  struct w_query_ctx ctx;
  struct write_locked_watchman_root wlock;
  struct read_locked_watchman_root rlock;
  bool result;

  memset(&ctx, 0, sizeof(ctx));
  ctx.query = query;

  memset(res, 0, sizeof(*res));

  w_perf_t sample("query_execute");

  if (query->fast_fail &&
      (unlocked->root->inner.should_recrawl ||
      !unlocked->root->inner.done_initial)) {
    ignore_result(asprintf(&res->errmsg, "recrawl is active\n"));
    return false;
  }

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
    w_clockspec_eval(&wlock, query->since_spec, &ctx.since);

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
    w_clockspec_eval_readonly(&rlock, query->since_spec, &ctx.since);
  }

  res->root_number = ctx.lock->root->inner.number;
  res->ticks = ctx.lock->root->inner.ticks;

  result = execute_common(&ctx, &sample, res, generator, gendata);
  // This handles the unlock in both the read and write case, as ctx.lock
  // points to the read or write lock as appropriate, and the underlying
  // unlock operation is defined to be safe for either.
  w_root_read_unlock(ctx.lock, unlocked);
  return result;
}

/* vim:ts=2:sw=2:et:
 */
