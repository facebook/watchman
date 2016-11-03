/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

// Helper functions for integer comparisons in query expressions

static const struct {
  const char *opname;
  enum w_query_icmp_op op;
} opname_to_op[] = {
  {"eq", W_QUERY_ICMP_EQ},
  {"ne", W_QUERY_ICMP_NE},
  {"gt", W_QUERY_ICMP_GT},
  {"ge", W_QUERY_ICMP_GE},
  {"lt", W_QUERY_ICMP_LT},
  {"le", W_QUERY_ICMP_LE},
};

// term is a json array that looks like:
// ["size", "eq", 1024]
bool parse_int_compare(
    const json_ref& term,
    struct w_query_int_compare* comp,
    char** errmsg) {
  const char *opname;
  size_t i;
  bool found = false;

  if (json_array_size(term) != 3) {
    ignore_result(asprintf(errmsg, "integer comparator must have 3 elements"));
    return false;
  }
  if (!json_is_string(json_array_get(term, 1))) {
    ignore_result(asprintf(errmsg, "integer comparator op must be a string"));
    return false;
  }
  if (!json_is_integer(json_array_get(term, 2))) {
    ignore_result(asprintf(errmsg,
          "integer comparator operand must be an integer"));
    return false;
  }

  opname = json_string_value(json_array_get(term, 1));
  for (i = 0; i < sizeof(opname_to_op)/sizeof(opname_to_op[0]); i++) {
    if (!strcmp(opname_to_op[i].opname, opname)) {
      comp->op = opname_to_op[i].op;
      found = true;
      break;
    }
  }

  if (!found) {
    ignore_result(asprintf(errmsg,
          "integer comparator opname `%s' is invalid",
          opname));
    return false;
  }


  comp->operand = json_integer_value(json_array_get(term, 2));
  return true;
}

bool eval_int_compare(json_int_t ival, struct w_query_int_compare *comp) {
  switch (comp->op) {
    case W_QUERY_ICMP_EQ:
      return ival == comp->operand;
    case W_QUERY_ICMP_NE:
      return ival != comp->operand;
    case W_QUERY_ICMP_GT:
      return ival > comp->operand;
    case W_QUERY_ICMP_GE:
      return ival >= comp->operand;
    case W_QUERY_ICMP_LT:
      return ival < comp->operand;
    case W_QUERY_ICMP_LE:
      return ival <= comp->operand;
    default:
      // Not possible to get here, but some compilers don't realize
      return false;
  }
}

class SizeExpr : public QueryExpr {
  w_query_int_compare comp;

 public:
  explicit SizeExpr(w_query_int_compare comp) : comp(comp) {}

  bool evaluate(struct w_query_ctx*, const watchman_file* file) override {
    // Removed files never evaluate true
    if (!file->exists) {
      return false;
    }

    return eval_int_compare(file->stat.size, &comp);
  }

  static std::unique_ptr<QueryExpr> parse(
      w_query* query,
      const json_ref& term) {
    if (!json_is_array(term)) {
      ignore_result(asprintf(&query->errmsg, "Expected array for 'size' term"));
      return nullptr;
    }

    w_query_int_compare comp;

    if (!parse_int_compare(term, &comp, &query->errmsg)) {
      return nullptr;
    }

    return watchman::make_unique<SizeExpr>(comp);
  }
};
W_TERM_PARSER("size", SizeExpr::parse)

/* vim:ts=2:sw=2:et:
 */
