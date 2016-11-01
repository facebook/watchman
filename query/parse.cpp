/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

// This can't be a simple global because other compilation units
// may try to mutate it before this compilation unit has had its
// constructors run, leading to SIOF.
static std::unordered_map<w_string, w_query_expr_parser>& term_hash() {
  static std::unordered_map<w_string, w_query_expr_parser> hash;
  return hash;
}

QueryExpr::~QueryExpr() {}

bool w_query_register_expression_parser(
    const char *term,
    w_query_expr_parser parser)
{
  char capname[128];
  w_string name(term, W_STRING_UNICODE);

  snprintf(capname, sizeof(capname), "term-%s", term);
  w_capability_register(capname);

  term_hash()[name] = parser;
  return true;
}

/* parse an expression term. It can be one of:
 * "term"
 * ["term" <parameters>]
 */
std::unique_ptr<QueryExpr> w_query_expr_parse(
    w_query* query,
    const json_ref& exp) {
  w_string name;

  if (json_is_string(exp)) {
    name = json_to_w_string(exp);
  } else if (json_is_array(exp) && json_array_size(exp) > 0) {
    const auto& first = exp.at(0);

    if (!json_is_string(first)) {
      query->errmsg = strdup(
          "first element of an expression must be a string");
      return NULL;
    }
    name = json_to_w_string(first);
  } else {
    query->errmsg = strdup("expected array or string for an expression");
    return NULL;
  }

  auto it = term_hash().find(name);
  if (it == term_hash().end()) {
    ignore_result(
        asprintf(&query->errmsg, "unknown expression term '%s'", name.c_str()));
    return NULL;
  }
  return it->second(query, exp);
}

static bool parse_since(w_query* res, const json_ref& query) {
  auto since = query.get_default("since");
  if (!since) {
    return true;
  }

  auto spec = w_clockspec_parse(since);
  if (spec) {
    // res owns the ref to spec
    res->since_spec = std::move(spec);
    return true;
  }

  res->errmsg = strdup("invalid value for 'since'");

  return false;
}

static w_string parse_suffix(w_query* res, const json_ref& ele) {
  if (!json_is_string(ele)) {
    res->errmsg = strdup("'suffix' must be a string or an array of strings");
    return nullptr;
  }

  auto str = json_to_w_string(ele);

  return w_string(w_string_new_lower_typed(str.c_str(), str.type()), false);
}

static bool parse_suffixes(w_query* res, const json_ref& query) {
  size_t i;

  auto suffixes = query.get_default("suffix");
  if (!suffixes) {
    return true;
  }

  if (json_is_string(suffixes)) {
    auto suff = parse_suffix(res, suffixes);
    if (!suff) {
      return false;
    }
    res->suffixes.emplace_back(std::move(suff));
    return true;
  }

  if (!json_is_array(suffixes)) {
    res->errmsg = strdup("'suffix' must be a string or an array of strings");
    return false;
  }

  res->suffixes.reserve(json_array_size(suffixes));

  for (i = 0; i < json_array_size(suffixes); i++) {
    const auto& ele = suffixes.at(i);

    if (!json_is_string(ele)) {
      res->errmsg = strdup("'suffix' must be a string or an array of strings");
      return false;
    }

    auto suff = parse_suffix(res, ele);
    if (!suff) {
      return false;
    }
    res->suffixes.emplace_back(std::move(suff));
  }

  return true;
}

static bool parse_paths(w_query* res, const json_ref& query) {
  size_t i;

  auto paths = query.get_default("path");
  if (!paths) {
    return true;
  }

  if (!json_is_array(paths)) {
    res->errmsg = strdup("'path' must be an array");
    return false;
  }

  res->paths.resize(json_array_size(paths));

  for (i = 0; i < json_array_size(paths); i++) {
    const auto& ele = paths.at(i);
    const char *name = NULL;

    res->paths[i].depth = -1;

    if (json_is_string(ele)) {
      name = json_string_value(ele);
    } else if (json_unpack(ele, "{s:s, s:i}",
          "path", &name,
          "depth", &res->paths[i].depth
          ) != 0) {
      res->errmsg = strdup(
          "expected object with 'path' and 'depth' properties"
          );
      return false;
    }

    auto nameCopy = w_string_new_typed(name, W_STRING_BYTE);
    w_string_in_place_normalize_separators(&nameCopy, WATCHMAN_DIR_SEP);
    res->paths[i].name = nameCopy;
  }

  return true;
}

W_CAP_REG("relative_root")

static bool
parse_relative_root(const w_root_t* root, w_query* res, const json_ref& query) {
  auto relative_root = query.get_default("relative_root");
  if (!relative_root) {
    return true;
  }

  if (!json_is_string(relative_root)) {
    res->errmsg = strdup("'relative_root' must be a string");
    return false;
  }

  w_string path = json_to_w_string(relative_root);
  w_string canon_path = w_string_canon_path(path);
  res->relative_root = w_string::pathCat({root->root_path, canon_path});
  res->relative_root_slash =
      w_string::printf("%s%c", res->relative_root.c_str(), WATCHMAN_DIR_SEP);

  return true;
}

static bool parse_query_expression(w_query* res, const json_ref& query) {
  auto exp = query.get_default("expression");
  if (!exp) {
    // Empty expression means that we emit all generated files
    return true;
  }

  res->expr = w_query_expr_parse(res, exp);
  if (!res->expr) {
    return false;
  }

  return true;
}

static bool parse_sync(w_query* res, const json_ref& query) {
  int value = DEFAULT_QUERY_SYNC_MS;

  if (query &&
      json_unpack(query, "{s?:i*}", "sync_timeout", &value) != 0) {
    res->errmsg = strdup("sync_timeout must be an integer value >= 0");
    return false;
  }

  if (value < 0) {
    res->errmsg = strdup("sync_timeout must be an integer value >= 0");
    return false;
  }

  res->sync_timeout = value;
  return true;
}

static bool parse_lock_timeout(w_query* res, const json_ref& query) {
  int value = DEFAULT_QUERY_SYNC_MS;

  if (query &&
      json_unpack(query, "{s?:i*}", "lock_timeout", &value) != 0) {
    res->errmsg = strdup("lock_timeout must be an integer value >= 0");
    return false;
  }

  if (value < 0) {
    res->errmsg = strdup("lock_timeout must be an integer value >= 0");
    return false;
  }

  res->lock_timeout = value;
  return true;
}

W_CAP_REG("dedup_results")

static bool parse_dedup(w_query* res, const json_ref& query) {
  int value = 0;

  if (query &&
      json_unpack(query, "{s?:b*}", "dedup_results", &value) != 0) {
    res->errmsg = strdup("dedup_results must be a boolean");
    return false;
  }

  res->dedup_results = (bool) value;
  return true;
}

static bool parse_empty_on_fresh_instance(w_query* res, const json_ref& query) {
  int value = 0;

  if (query &&
      json_unpack(query, "{s?:b*}", "empty_on_fresh_instance", &value) != 0) {
    res->errmsg = strdup("empty_on_fresh_instance must be a boolean");
    return false;
  }

  res->empty_on_fresh_instance = (bool) value;
  return true;
}

static bool parse_case_sensitive(
    w_query* res,
    const w_root_t* root,
    const json_ref& query) {
  int value = root->case_sensitive;

  if (query && json_unpack(query, "{s?:b*}", "case_sensitive", &value) != 0) {
    res->errmsg = strdup("case_sensitive must be a boolean");
    return false;
  }

  res->case_sensitive = (bool) value;
  return true;
}

std::shared_ptr<w_query>
w_query_parse(const w_root_t* root, const json_ref& query, char** errmsg) {
  auto result = std::make_shared<w_query>();
  auto res = result.get();

  *errmsg = NULL;

  if (!parse_case_sensitive(res, root, query)) {
    goto error;
  }

  if (!parse_sync(res, query)) {
    goto error;
  }

  if (!parse_dedup(res, query)) {
    goto error;
  }

  if (!parse_lock_timeout(res, query)) {
    goto error;
  }

  if (!parse_relative_root(root, res, query)) {
    goto error;
  }

  if (!parse_empty_on_fresh_instance(res, query)) {
    goto error;
  }

  /* Look for path generators */
  if (!parse_paths(res, query)) {
    goto error;
  }

  /* Look for glob generators */
  if (!parse_globs(res, query)) {
    goto error;
  }

  /* Look for suffix generators */
  if (!parse_suffixes(res, query)) {
    goto error;
  }

  /* Look for since generator */
  if (!parse_since(res, query)) {
    goto error;
  }

  if (!parse_query_expression(res, query)) {
    goto error;
  }

  res->query_spec = query;

  return result;
error:
  if (res) {
    *errmsg = res->errmsg;
    res->errmsg = NULL;
  }
  if (!*errmsg) {
    *errmsg = strdup("unspecified error");
  }

  return nullptr;
}

bool w_query_legacy_field_list(struct w_query_field_list *flist)
{
  static const char *names[] = {
    "name", "exists", "size", "mode", "uid", "gid", "mtime",
    "ctime", "ino", "dev", "nlink", "new", "cclock", "oclock"
  };
  uint8_t i;
  auto list = json_array();
  bool res;
  char *errmsg = NULL;

  for (i = 0; i < sizeof(names)/sizeof(names[0]); i++) {
    json_array_append_new(list, typed_string_to_json(names[i],
        W_STRING_UNICODE));
  }

  res = parse_field_list(list, flist, &errmsg);

  if (errmsg) {
    w_log(W_LOG_FATAL, "should never happen: %s\n", errmsg);
  }

  return res;
}

// Translate from the legacy array into the new style, then
// delegate to the main parser.
// We build a big anyof expression
std::shared_ptr<w_query> w_query_parse_legacy(
    const w_root_t* root,
    const json_ref& args,
    char** errmsg,
    int start,
    uint32_t* next_arg,
    const char* clockspec,
    json_ref* expr_p) {
  bool include = true;
  bool negated = false;
  uint32_t i;
  const char *term_name = "match";
  json_ref included, excluded;
  auto query_obj = json_object();

  if (!json_is_array(args)) {
    *errmsg = strdup("Expected an array");
    return NULL;
  }

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!arg) {
      /* not a string value! */
      ignore_result(asprintf(errmsg,
          "rule @ position %d is not a string value", i));
      return NULL;
    }
  }

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }
    if (!strcmp(arg, "-P")) {
      term_name = "ipcre";
      continue;
    }
    if (!strcmp(arg, "-p")) {
      term_name = "pcre";
      continue;
    }

    // Which group are we going to file it into
    json_ref container;
    if (include) {
      if (!included) {
        included =
            json_array({typed_string_to_json("anyof", W_STRING_UNICODE)});
      }
      container = included;
    } else {
      if (!excluded) {
        excluded =
            json_array({typed_string_to_json("anyof", W_STRING_UNICODE)});
      }
      container = excluded;
    }

    auto term =
        json_array({typed_string_to_json(term_name, W_STRING_UNICODE),
                    typed_string_to_json(arg),
                    typed_string_to_json("wholename", W_STRING_UNICODE)});
    if (negated) {
      term = json_array({typed_string_to_json("not", W_STRING_UNICODE), term});
    }
    json_array_append_new(container, std::move(term));

    // Reset negated flag
    negated = false;
    term_name = "match";
  }

  if (excluded) {
    excluded =
        json_array({typed_string_to_json("not", W_STRING_UNICODE), excluded});
  }

  json_ref query_array;
  if (included && excluded) {
    query_array = json_array(
        {typed_string_to_json("allof", W_STRING_UNICODE), excluded, included});
  } else if (included) {
    query_array = included;
  } else {
    query_array = excluded;
  }

  // query_array may be NULL, which means find me all files.
  // Otherwise, it is the expression we want to use.
  if (query_array) {
    json_object_set_new_nocheck(
        query_obj, "expression", std::move(query_array));
  }

  // For trigger
  if (next_arg) {
    *next_arg = i;
  }

  if (clockspec) {
    json_object_set_new_nocheck(query_obj,
        "since", typed_string_to_json(clockspec, W_STRING_UNICODE));
  }

  /* compose the query with the field list */
  auto query = w_query_parse(root, query_obj, errmsg);

  if (expr_p) {
    *expr_p = query_obj;
  }

  return query;
}

/* vim:ts=2:sw=2:et:
 */
