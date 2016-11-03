/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "make_unique.h"

enum since_what { SINCE_OCLOCK, SINCE_CCLOCK, SINCE_MTIME, SINCE_CTIME };

static struct {
  enum since_what value;
  const char* label;
} allowed_fields[] = {
    {since_what::SINCE_OCLOCK, "oclock"},
    {since_what::SINCE_CCLOCK, "cclock"},
    {since_what::SINCE_MTIME, "mtime"},
    {since_what::SINCE_CTIME, "ctime"},
};

class SinceExpr : public QueryExpr {
  std::unique_ptr<w_clockspec> spec;
  enum since_what field;

 public:
  explicit SinceExpr(std::unique_ptr<w_clockspec> spec, enum since_what field)
      : spec(std::move(spec)), field(field) {}

  bool evaluate(struct w_query_ctx* ctx, const watchman_file* file) override {
    w_clock_t clock;
    struct w_query_since since;
    time_t tval = 0;

    w_clockspec_eval(ctx->lock, spec.get(), &since);

    switch (field) {
      case since_what::SINCE_OCLOCK:
      case since_what::SINCE_CCLOCK:
        clock = (field == since_what::SINCE_OCLOCK) ? file->otime : file->ctime;
        if (since.is_timestamp) {
          return since.timestamp > clock.timestamp;
        }
        if (since.clock.is_fresh_instance) {
          return file->exists;
        }
        return clock.ticks > since.clock.ticks;
      case since_what::SINCE_MTIME:
        tval = file->stat.mtime.tv_sec;
        break;
      case since_what::SINCE_CTIME:
        tval = file->stat.ctime.tv_sec;
        break;
    }

    assert(since.is_timestamp);
    return tval > since.timestamp;
  }

  static std::unique_ptr<QueryExpr> parse(
      w_query* query,
      const json_ref& term) {
    std::unique_ptr<w_clockspec> spec;
    auto selected_field = since_what::SINCE_OCLOCK;
    const char* fieldname = "oclock";

    if (!json_is_array(term)) {
      query->errmsg = strdup("\"since\" term must be an array");
      return nullptr;
    }

    if (json_array_size(term) < 2 || json_array_size(term) > 3) {
      query->errmsg = strdup("\"since\" term has invalid number of parameters");
      return nullptr;
    }

    const auto& jval = term.at(1);
    spec = w_clockspec_parse(jval);
    if (!spec) {
      query->errmsg = strdup("invalid clockspec for \"since\" term");
      return nullptr;
    }
    if (spec->tag == w_cs_named_cursor) {
      query->errmsg =
          strdup("named cursors are not allowed in \"since\" terms");
      return nullptr;
    }

    if (term.array().size() == 3) {
      const auto& field = term.at(2);
      size_t i;
      bool valid = false;

      fieldname = json_string_value(field);
      if (!fieldname) {
        query->errmsg =
            strdup("field name for \"since\" term must be a string");
        return nullptr;
      }

      for (i = 0; i < sizeof(allowed_fields) / sizeof(allowed_fields[0]); ++i) {
        if (!strcmp(allowed_fields[i].label, fieldname)) {
          selected_field = allowed_fields[i].value;
          valid = true;
          break;
        }
      }

      if (!valid) {
        ignore_result(asprintf(
            &query->errmsg,
            "invalid field name \"%s\" for \"since\" term",
            fieldname));
        return nullptr;
      }
    }

    switch (selected_field) {
      case since_what::SINCE_CTIME:
      case since_what::SINCE_MTIME:
        if (spec->tag != w_cs_timestamp) {
          ignore_result(asprintf(
              &query->errmsg,
              "field \"%s\" requires a timestamp value "
              "for comparison in \"since\" term",
              fieldname));
          return nullptr;
        }
        break;
      case since_what::SINCE_OCLOCK:
      case since_what::SINCE_CCLOCK:
        /* we'll work with clocks or timestamps */
        break;
    }

    return watchman::make_unique<SinceExpr>(std::move(spec), selected_field);
  }
};
W_TERM_PARSER("since", SinceExpr::parse)

/* vim:ts=2:sw=2:et:
 */
