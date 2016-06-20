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

w_string_t *w_query_ctx_get_wholename(
    struct w_query_ctx *ctx
)
{
  w_string_t *full_name;
  uint32_t name_start;

  if (ctx->wholename) {
    return ctx->wholename;
  }

  if (ctx->query->relative_root != NULL) {
    // At this point every path should start with the relative root, so this is
    // legal
    name_start = ctx->query->relative_root->len + 1;
  } else {
    name_start = ctx->root->root_path->len + 1;
  }

  full_name = w_string_path_cat(ctx->file->parent->path, ctx->file->name);
  // Record the name relative to the root
  ctx->wholename = w_string_slice(full_name, name_start,
      full_name->len - name_start);
  w_string_delref(full_name);

  return ctx->wholename;
}

bool w_query_process_file(
    w_query *query,
    struct w_query_ctx *ctx,
    struct watchman_file *file)
{
  struct watchman_rule_match *m;

  if (ctx->wholename) {
    w_string_delref(ctx->wholename);
    ctx->wholename = NULL;
  }
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

  // Need more room?
  if (ctx->num_results + 1 > ctx->num_allocd) {
    uint32_t new_num = ctx->num_allocd ? ctx->num_allocd * 2 : 64;
    struct watchman_rule_match *res;

    res = realloc(ctx->results, new_num * sizeof(*res));
    if (!res) {
      w_log(W_LOG_ERR, "out of memory while capturing matches!\n");
      return false;
    }

    ctx->results = res;
    ctx->num_allocd = new_num;
  }

  m = &ctx->results[ctx->num_results++];

  m->root_number = ctx->root->number;
  m->relname = w_query_ctx_get_wholename(ctx);
  if (!m->relname) {
    w_log(W_LOG_ERR, "out of memory while capturing matches!\n");
    return false;
  }
  w_string_addref(m->relname);

  m->file = file;
  if (ctx->since.is_timestamp) {
    m->is_new = w_timeval_compare(ctx->since.timestamp, file->ctime.tv) > 0;
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
  if (ctx->query->relative_root == NULL) {
    return true;
  }

  parent_path = f->parent->path;
  // "in relative root" here does not mean exactly the relative root, so compare
  // against the relative root's parent.
  return w_string_equal(parent_path, ctx->query->relative_root)
    || w_string_startswith(parent_path, ctx->query->relative_root_slash);
}

static bool time_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx)
{
  struct watchman_file *f;

  // Walk back in time until we hit the boundary
  for (f = root->latest_file; f; f = f->next) {
    if (ctx->since.is_timestamp &&
        w_timeval_compare(f->otime.tv, ctx->since.timestamp) < 0) {
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
      return false;
    }
  }

  return true;
}

static bool suffix_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx)
{
  uint32_t i;
  struct watchman_file *f;

  for (i = 0; i < query->nsuffixes; i++) {
    // Head of suffix index for this suffix
    f = w_ht_val_ptr(w_ht_get(root->suffixes,
          w_ht_ptr_val(query->suffixes[i])));


    // Walk and process
    for (; f; f = f->suffix_next) {
      if (!w_query_file_matches_relative_root(ctx, f)) {
        continue;
      }

      if (!w_query_process_file(query, ctx, f)) {
        return false;
      }
    }
  }
  return true;
}

static bool all_files_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx)
{
  struct watchman_file *f;

  for (f = root->latest_file; f; f = f->next) {
    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    if (!w_query_process_file(query, ctx, f)) {
      return false;
    }
  }
  return true;
}

static bool dir_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx,
    struct watchman_dir *dir,
    uint32_t depth)
{
  w_ht_iter_t i;

  if (w_ht_first(dir->files, &i)) do {
    struct watchman_file *file = w_ht_val_ptr(i.value);

    if (!w_query_process_file(query, ctx, file)) {
      return false;
    }
  } while (w_ht_next(dir->files, &i));

  if (depth > 0 && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = w_ht_val_ptr(i.value);

    if (!dir_generator(query, root, ctx, child, depth - 1)) {
      return false;
    }
  } while (w_ht_next(dir->dirs, &i));

  return true;
}

static bool path_generator(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx)
{
  w_string_t *relative_root;
  struct watchman_file *f;
  uint32_t i;

  if (query->relative_root != NULL) {
    relative_root = query->relative_root;
  } else {
    relative_root = root->root_path;
  }

  for (i = 0; i < query->npaths; i++) {
    struct watchman_dir *dir;
    w_string_t *dir_name, *file_name, *full_name;

    // Compose path with root
    full_name = w_string_path_cat(relative_root, query->paths[i].name);

    // special case of root dir itself
    if (w_string_equal(root->root_path, full_name)) {
      // dirname on the root is outside the root, which is useless
      dir = w_root_resolve_dir(root, full_name, false);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = w_string_dirname(full_name);
    if (!dir_name) {
      w_string_delref(full_name);
      continue;
    }

    dir = w_root_resolve_dir(root, dir_name, false);
    w_string_delref(dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      w_string_delref(full_name);
      continue;
    }

    if (dir->files) {
      file_name = w_string_basename(query->paths[i].name);
      f = w_ht_val_ptr(w_ht_get(dir->files, w_ht_ptr_val(file_name)));
      w_string_delref(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !S_ISDIR(f->stat.mode))) {
        w_string_delref(full_name);
        if (!w_query_process_file(query, ctx, f)) {
          return false;
        }
        continue;
      }
    }

    // Is it a dir?
    if (!dir->dirs) {
      w_string_delref(full_name);
      continue;
    }

    dir = w_ht_val_ptr(w_ht_get(dir->dirs, w_ht_ptr_val(full_name)));
    w_string_delref(full_name);
is_dir:
    // We got a dir; process recursively to specified depth
    if (dir && !dir_generator(query, root, ctx, dir,
          query->paths[i].depth)) {
      return false;
    }
  }

  return true;
}

static bool default_generators(
    w_query *query,
    w_root_t *root,
    struct w_query_ctx *ctx,
    void *gendata)
{
  bool generated = false;

  unused_parameter(gendata);

  // Time based query
  if (ctx->since.is_timestamp || !ctx->since.clock.is_fresh_instance) {
    if (!time_generator(query, root, ctx)) {
      return false;
    }
    generated = true;
  }

  // Suffix
  if (query->suffixes) {
    if (!suffix_generator(query, root, ctx)) {
      return false;
    }
    generated = true;
  }

  if (query->npaths) {
    if (!path_generator(query, root, ctx)) {
      return false;
    }
    generated = true;
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (!generated) {
    if (!all_files_generator(query, root, ctx)) {
      return false;
    }
  }

  return true;
}

void w_query_result_free(w_query_res *res)
{
  free(res->errmsg);
  res->errmsg = NULL;
  w_match_results_free(res->num_results, res->results);
  res->results = NULL;
}

bool w_query_execute(
    w_query *query,
    w_root_t *root,
    w_query_res *res,
    w_query_generator generator,
    void *gendata)
{
  struct w_query_ctx ctx;
  w_perf_t sample;

  memset(&ctx, 0, sizeof(ctx));
  ctx.query = query;
  ctx.root = root;

  memset(res, 0, sizeof(*res));

  w_perf_start(&sample, "query_execute");

  if (query->sync_timeout && !w_root_sync_to_now(root, query->sync_timeout)) {
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

  // Lock the root and begin generation
  if (!w_root_lock_with_timeout(root, "w_query_execute", query->lock_timeout)) {
    ignore_result(asprintf(&res->errmsg, "couldn't acquire root lock within "
                                         "lock_timeout of %dms. root is "
                                         "currently busy (%s)\n",
                           query->lock_timeout, root->lock_reason));
    return false;
  }
  res->root_number = root->number;
  res->ticks = root->ticks;

  // Evaluate the cursor for this root
  w_clockspec_eval(root, query->since_spec, &ctx.since);

  res->is_fresh_instance = !ctx.since.is_timestamp &&
    ctx.since.clock.is_fresh_instance;

  if (!(res->is_fresh_instance && query->empty_on_fresh_instance)) {
    if (!generator) {
      generator = default_generators;
    }

    generator(query, root, &ctx, gendata);
  }

  if (w_perf_finish(&sample)) {
    w_perf_add_root_meta(&sample, root);
    w_perf_add_meta(&sample, "query_execute",
                    json_pack("{s:b, s:i}",                             //
                              "fresh_instance", res->is_fresh_instance, //
                              "num_results", ctx.num_results            //
                              ));
    w_perf_log(&sample);
  }
  w_root_unlock(root);
  w_perf_destroy(&sample);

  if (ctx.wholename) {
    w_string_delref(ctx.wholename);
  }
  res->results = ctx.results;
  res->num_results = ctx.num_results;

  return true;
}

/* vim:ts=2:sw=2:et:
 */
