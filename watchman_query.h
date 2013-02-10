/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_QUERY_H
#define WATCHMAN_QUERY_H

#ifdef __cplusplus
extern "C" {
#endif

struct w_query;
typedef struct w_query w_query;

struct w_query_expr;
typedef struct w_query_expr w_query_expr;

// Holds state for the execution of a query
struct w_query_ctx {
  struct w_query *query;
  w_root_t *root;
  struct watchman_file *file;
  w_string_t *wholename;
  struct w_clockspec_query since;

  struct watchman_rule_match *results;
  uint32_t num_results;
  uint32_t num_allocd;
};

struct w_query_path {
  w_string_t *name;
  int depth;
};

typedef bool (*w_query_expr_eval_func)(
    struct w_query_ctx *ctx,
    struct watchman_file *file,
    void *data
);
typedef void (*w_query_expr_dispose_func)(
    void *data
);
struct w_query_expr {
  int refcnt;
  w_query_expr_eval_func    evaluate;
  w_query_expr_dispose_func dispose;
  void *data;
};

struct w_query {
  int refcnt;

  // Indicates that we generate for all files.
  // This is only set iff there are no other
  // generators
  bool all_files;

  struct w_query_path *paths;
  uint32_t npaths;

  w_string_t **suffixes;
  uint32_t nsuffixes;

  // We can't (and mustn't!) evaluate the clockspec
  // fully until we execute query, because we have
  // to evaluate named cursors at the time we execute
  json_t *since;

  w_query_expr *expr;

  // Error message placeholder while parsing
  char *errmsg;
};

typedef w_query_expr *(*w_query_expr_parser)(
    w_query *query,
    json_t *term
);

bool w_query_register_expression_parser(
    const char *term,
    w_query_expr_parser parser);

w_query *w_query_parse(json_t *query, char **errmsg);
void w_query_delref(w_query *query);

w_query_expr *w_query_expr_parse(w_query *query, json_t *term);

void w_query_expr_delref(w_query_expr *expr);
void w_query_expr_addref(w_query_expr *expr);
w_query_expr *w_query_expr_new(
    w_query_expr_eval_func evaluate,
    w_query_expr_dispose_func dispose,
    void *data
);

uint32_t w_query_execute(
    w_query *query,
    w_root_t *root,
    uint32_t *ticks,
    struct watchman_rule_match **results);


// Returns a shared reference to the wholename
// of the file.  The caller must not delref
// the reference.
w_string_t *w_query_ctx_get_wholename(
    struct w_query_ctx *ctx
);

bool w_query_expr_evaluate(
    w_query_expr *expr,
    struct w_query_ctx *ctx,
    struct watchman_file *file);

struct w_query_field_renderer;
struct w_query_field_list {
  unsigned int num_fields;
  struct w_query_field_renderer *fields[32];
};

json_t *w_query_results_to_json(
    struct w_query_field_list *field_list,
    uint32_t num_results,
    struct watchman_rule_match *results);

void w_query_init_all(void);

w_query_expr *w_expr_true_parser(w_query *query, json_t *term);
w_query_expr *w_expr_false_parser(w_query *query, json_t *term);
w_query_expr *w_expr_anyof_parser(w_query *query, json_t *term);
w_query_expr *w_expr_allof_parser(w_query *query, json_t *term);
w_query_expr *w_expr_not_parser(w_query *query, json_t *term);
w_query_expr *w_expr_type_parser(w_query *query, json_t *term);
w_query_expr *w_expr_suffix_parser(w_query *query, json_t *term);
w_query_expr *w_expr_match_parser(w_query *query, json_t *term);
w_query_expr *w_expr_imatch_parser(w_query *query, json_t *term);
w_query_expr *w_expr_pcre_parser(w_query *query, json_t *term);
w_query_expr *w_expr_ipcre_parser(w_query *query, json_t *term);
w_query_expr *w_expr_name_parser(w_query *query, json_t *term);
w_query_expr *w_expr_iname_parser(w_query *query, json_t *term);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

