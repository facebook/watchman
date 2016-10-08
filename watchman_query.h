/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_QUERY_H
#define WATCHMAN_QUERY_H
#include <deque>

struct w_query;
typedef struct w_query w_query;

struct w_query_expr;
typedef struct w_query_expr w_query_expr;

struct w_query_since {
  bool is_timestamp;
  union {
    time_t timestamp;
    struct {
      bool is_fresh_instance;
      uint32_t ticks;
    } clock;
  };
};

struct watchman_rule_match {
  uint32_t root_number;
  w_string relname;
  bool is_new;
  struct watchman_file *file;

  watchman_rule_match(
      uint32_t root_number,
      const w_string& relname,
      bool is_new,
      watchman_file* file)
      : root_number(root_number),
        relname(relname),
        is_new(is_new),
        file(file) {}
};

// Holds state for the execution of a query
struct w_query_ctx {
  struct w_query *query;
  struct read_locked_watchman_root *lock;
  struct watchman_file *file;
  w_string wholename;
  struct w_query_since since;

  std::deque<watchman_rule_match> results;

  // Cache for dir name lookups when computing wholename
  watchman_dir *last_parent;
  w_string_t *last_parent_path;

  // When deduping the results, effectively a set<wholename> of
  // the files held in results
  w_ht_t *dedup;

  // How many times we suppressed a result due to dedup checking
  uint32_t num_deduped;
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
  long refcnt;
  w_query_expr_eval_func    evaluate;
  w_query_expr_dispose_func dispose;
  void *data;
};

struct watchman_glob_tree;

struct w_query {
  long refcnt;

  bool case_sensitive;
  bool empty_on_fresh_instance;
  bool dedup_results;

  /* optional full path to relative root, without and with trailing slash */
  w_string_t *relative_root;
  w_string_t *relative_root_slash;

  struct w_query_path *paths;
  size_t npaths;

  struct watchman_glob_tree *glob_tree;
  // Additional flags to pass to wildmatch in the glob_generator
  int glob_flags;

  w_string_t **suffixes;
  size_t nsuffixes;

  uint32_t sync_timeout;
  uint32_t lock_timeout;

  // We can't (and mustn't!) evaluate the clockspec
  // fully until we execute query, because we have
  // to evaluate named cursors and determine fresh
  // instance at the time we execute
  struct w_clockspec *since_spec;

  w_query_expr *expr;

  // Error message placeholder while parsing
  char *errmsg;

  // The query that we parsed into this struct
  json_t *query_spec;
};

typedef w_query_expr *(*w_query_expr_parser)(
    w_query *query,
    json_t *term
);

bool w_query_register_expression_parser(
    const char *term,
    w_query_expr_parser parser);

w_query *w_query_parse(const w_root_t *root, json_t *query, char **errmsg);
void w_query_delref(w_query *query);

w_query_expr *w_query_expr_parse(w_query *query, json_t *term);

void w_query_expr_delref(w_query_expr *expr);
void w_query_expr_addref(w_query_expr *expr);
w_query_expr *w_query_expr_new(
    w_query_expr_eval_func evaluate,
    w_query_expr_dispose_func dispose,
    void *data
);

bool w_query_file_matches_relative_root(
    struct w_query_ctx *ctx,
    struct watchman_file *file);

// Allows a generator to process a file node
// through the query engine
bool w_query_process_file(
    w_query *query,
    struct w_query_ctx *ctx,
    struct watchman_file *file);

// Generator callback, used to plug in an alternate
// generator when used in triggers or subscriptions
typedef bool (*w_query_generator)(w_query *query,
                                  struct read_locked_watchman_root *lock,
                                  struct w_query_ctx *ctx, void *gendata,
                                  int64_t *num_walked);

struct w_query_result {
  bool is_fresh_instance;
  std::deque<watchman_rule_match> results;
  uint32_t root_number;
  uint32_t ticks;
  char* errmsg{nullptr};

  ~w_query_result();
};
typedef struct w_query_result w_query_res;

bool w_query_execute(
    w_query *query,
    struct unlocked_watchman_root *unlocked,
    w_query_res *results,
    w_query_generator generator,
    void *gendata
);

bool w_query_execute_locked(
    w_query *query,
    struct write_locked_watchman_root *lock,
    w_query_res *results,
    w_query_generator generator,
    void *gendata
);


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

// parse the old style since and find queries
w_query *w_query_parse_legacy(const w_root_t *root, json_t *args, char **errmsg,
    int start, uint32_t *next_arg, const char *clockspec, json_t **expr_p);
bool w_query_legacy_field_list(struct w_query_field_list *flist);

json_t* w_query_results_to_json(
    struct w_query_field_list* field_list,
    uint32_t num_results,
    const std::deque<watchman_rule_match>& results);

void w_query_init_all(void);

enum w_query_icmp_op {
  W_QUERY_ICMP_EQ,
  W_QUERY_ICMP_NE,
  W_QUERY_ICMP_GT,
  W_QUERY_ICMP_GE,
  W_QUERY_ICMP_LT,
  W_QUERY_ICMP_LE,
};
struct w_query_int_compare {
  enum w_query_icmp_op op;
  json_int_t operand;
};
bool parse_int_compare(json_t *term, struct w_query_int_compare *comp,
    char **errmsg);
bool eval_int_compare(json_int_t ival, struct w_query_int_compare *comp);

bool parse_field_list(json_t *field_list,
    struct w_query_field_list *selected,
    char **errmsg);

bool glob_generator(w_query *query, struct read_locked_watchman_root *lock,
                    struct w_query_ctx *ctx, int64_t *num_walked);
bool parse_globs(w_query *res, json_t *query);
void free_glob_tree(struct watchman_glob_tree *glob_tree);

#define W_TERM_PARSER1(symbol, name, func) \
  static w_ctor_fn_type(symbol) {                   \
    w_query_register_expression_parser(name, func); \
  }                                                 \
  w_ctor_fn_reg(symbol)

#define W_TERM_PARSER(name, func) \
  W_TERM_PARSER1(w_gen_symbol(w_term_register_), name, func)

#endif

/* vim:ts=2:sw=2:et:
 */
