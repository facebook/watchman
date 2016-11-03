/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

class TypeExpr : public QueryExpr {
  char arg;

 public:
  explicit TypeExpr(char arg) : arg(arg) {}

  bool evaluate(struct w_query_ctx*, const watchman_file* file) override {
    switch (arg) {
      case 'b':
        return S_ISBLK(file->stat.mode);
      case 'c':
        return S_ISCHR(file->stat.mode);
      case 'd':
        return S_ISDIR(file->stat.mode);
      case 'f':
        return S_ISREG(file->stat.mode);
      case 'p':
        return S_ISFIFO(file->stat.mode);
      case 'l':
        return S_ISLNK(file->stat.mode);
      case 's':
        return S_ISSOCK(file->stat.mode);
#ifdef S_ISDOOR
    case 'D':
      return S_ISDOOR(file->stat.mode);
#endif
    default:
      return false;
    }
  }

  static std::unique_ptr<QueryExpr> parse(
      w_query* query,
      const json_ref& term) {
    const char *ignore, *typestr, *found;
    char arg;

    if (json_unpack(term, "[s,u]", &ignore, &typestr) != 0) {
      query->errmsg = strdup("must use [\"type\", \"typestr\"]");
      return nullptr;
    }

    found = strpbrk(typestr, "bcdfplsD");
    if (!found || strlen(typestr) > 1) {
      ignore_result(
          asprintf(&query->errmsg, "invalid type string '%s'", typestr));
      return nullptr;
    }

    arg = *found;

    return watchman::make_unique<TypeExpr>(arg);
  }
};
W_TERM_PARSER("type", TypeExpr::parse)

/* vim:ts=2:sw=2:et:
 */
