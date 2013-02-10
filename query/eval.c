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

  name_start = ctx->root->root_path->len + 1;

  full_name = w_string_path_cat(ctx->file->parent->path, ctx->file->name);
  // Record the name relative to the root
  ctx->wholename = w_string_slice(full_name, name_start,
      full_name->len - name_start);
  w_string_delref(full_name);

  return ctx->wholename;
}

static bool process_file(
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

  m->relname = w_query_ctx_get_wholename(ctx);
  if (!m->relname) {
    w_log(W_LOG_ERR, "out of memory while capturing matches!\n");
    return false;
  }
  w_string_addref(m->relname);

  m->file = file;
  m->is_new = false;
  if (query->since) {
    if (!ctx->since.is_timestamp) {
      m->is_new = file->ctime.ticks > ctx->since.ticks;
    } else {
      m->is_new = w_timeval_compare(ctx->since.tv, file->ctime.tv) > 0;
    }
  }

  return true;
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
        w_timeval_compare(f->otime.tv, ctx->since.tv) < 0) {
      break;
    }
    if (!ctx->since.is_timestamp &&
        f->otime.ticks < ctx->since.ticks) {
      break;
    }

    if (!process_file(query, ctx, f)) {
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
    f = (void*)w_ht_get(root->suffixes, (w_ht_val_t)query->suffixes[i]);

    // Walk and process
    while (f) {
      if (!process_file(query, ctx, f)) {
        return false;
      }
      f = f->suffix_next;
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
    if (!process_file(query, ctx, f)) {
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
    struct watchman_file *file = (struct watchman_file*)i.value;

    if (!process_file(query, ctx, file)) {
      return false;
    }
  } while (w_ht_next(dir->files, &i));

  if (depth > 0 && w_ht_first(dir->dirs, &i)) do {
    struct watchman_dir *child = (struct watchman_dir*)i.value;

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
  struct watchman_file *f;
  uint32_t i;

  for (i = 0; i < query->npaths; i++) {
    struct watchman_dir *dir;
    w_string_t *dir_name, *file_name, *full_name;

    // Compose path with root
    full_name = w_string_path_cat(root->root_path, query->paths[i].name);

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
      f = (struct watchman_file*)w_ht_get(dir->files, (w_ht_val_t)file_name);
      w_string_delref(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !S_ISDIR(f->st.st_mode))) {
        w_string_delref(full_name);
        if (!process_file(query, ctx, f)) {
          return false;
        }
        continue;
      }
    }

    // Is it a dir?
    dir = (struct watchman_dir*)w_ht_get(dir->dirs,
            (w_ht_val_t)full_name);
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


uint32_t w_query_execute(
    w_query *query,
    w_root_t *root,
    uint32_t *ticks,
    struct watchman_rule_match **results)
{
  struct w_query_ctx ctx;

  memset(&ctx, 0, sizeof(ctx));
  ctx.query = query;
  ctx.root = root;

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

  // Evaluate the cursor; this may establish a lock on root
  if (query->since) {
    w_parse_clockspec(root, query->since, &ctx.since);
  }

  // Now we can lock the root and begin generation
  w_root_lock(root);
  *ticks = root->ticks;

  // Time based query
  if (query->since) {
    if (!time_generator(query, root, &ctx)) {
      goto done;
    }
  }

  // Suffix
  if (query->suffixes) {
    if (!suffix_generator(query, root, &ctx)) {
      goto done;
    }
  }

  if (query->npaths) {
    if (!path_generator(query, root, &ctx)) {
      goto done;
    }
  }

  // And finally, if there were no other generators, we walk all known
  // files
  if (query->all_files) {
    if (!all_files_generator(query, root, &ctx)) {
      goto done;
    }
  }

done:
  w_root_unlock(root);

  if (ctx.wholename) {
    w_string_delref(ctx.wholename);
  }
  *results = ctx.results;

  return ctx.num_results;
}



/* vim:ts=2:sw=2:et:
 */

