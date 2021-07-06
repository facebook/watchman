/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/watchman.h"

#include <memory>

using namespace watchman;

// Helper functions for integer comparisons in query expressions

static const struct {
  const char* opname;
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
void parse_int_compare(const json_ref& term, struct w_query_int_compare* comp) {
  const char* opname;
  size_t i;
  bool found = false;

  if (json_array_size(term) != 3) {
    throw QueryParseError("integer comparator must have 3 elements");
  }
  if (!json_array_get(term, 1).isString()) {
    throw QueryParseError("integer comparator op must be a string");
  }
  if (!json_array_get(term, 2).isInt()) {
    throw QueryParseError("integer comparator operand must be an integer");
  }

  opname = json_string_value(json_array_get(term, 1));
  for (i = 0; i < sizeof(opname_to_op) / sizeof(opname_to_op[0]); i++) {
    if (!strcmp(opname_to_op[i].opname, opname)) {
      comp->op = opname_to_op[i].op;
      found = true;
      break;
    }
  }

  if (!found) {
    throw QueryParseError(folly::to<std::string>(
        "integer comparator opname `", opname, "' is invalid"));
  }

  comp->operand = json_array_get(term, 2).asInt();
}

bool eval_int_compare(json_int_t ival, struct w_query_int_compare* comp) {
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

  EvaluateResult evaluate(struct w_query_ctx*, FileResult* file) override {
    auto exists = file->exists();
    auto size = file->size();

    if (!exists.has_value()) {
      return folly::none;
    }

    // Removed files never match
    if (!exists.value()) {
      return false;
    }

    if (!size.has_value()) {
      return folly::none;
    }

    return eval_int_compare(size.value(), &comp);
  }

  static std::unique_ptr<QueryExpr> parse(w_query*, const json_ref& term) {
    if (!term.isArray()) {
      throw QueryParseError("Expected array for 'size' term");
    }

    w_query_int_compare comp;
    parse_int_compare(term, &comp);

    return std::make_unique<SizeExpr>(comp);
  }
};
W_TERM_PARSER("size", SizeExpr::parse)

/* vim:ts=2:sw=2:et:
 */
