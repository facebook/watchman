/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

static json_ref make_name(const struct watchman_rule_match* match) {
  return w_string_to_json(match->relname);
}

static json_ref make_symlink(const struct watchman_rule_match* match) {
  return (match->file->symlink_target) ?
    w_string_to_json(match->file->symlink_target) : json_null();
}

static json_ref make_exists(const struct watchman_rule_match* match) {
  return json_boolean(match->file->exists);
}

static json_ref make_new(const struct watchman_rule_match* match) {
  return json_boolean(match->is_new);
}

#define MAKE_CLOCK_FIELD(name, member)                                   \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    char buf[128];                                                       \
    if (clock_id_string(                                                 \
            match->root_number,                                          \
            match->file->member.ticks,                                   \
            buf,                                                         \
            sizeof(buf))) {                                              \
      return typed_string_to_json(buf, W_STRING_UNICODE);                \
    }                                                                    \
    return nullptr;                                                      \
  }
MAKE_CLOCK_FIELD(cclock, ctime)
MAKE_CLOCK_FIELD(oclock, otime)

// Note: our JSON library supports 64-bit integers, but this may
// pose a compatibility issue for others.  We'll see if anyone
// runs into an issue and deal with it then...
#define MAKE_INT_FIELD(name, member)                                     \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    return json_integer(match->file->stat.member);                       \
  }

#define MAKE_TIME_INT_FIELD(name, type, scale)                           \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    struct timespec spec = match->file->stat.type##time;                 \
    return json_integer(                                                 \
        ((int64_t)spec.tv_sec * scale) +                                 \
        ((int64_t)spec.tv_nsec * scale / WATCHMAN_NSEC_IN_SEC));         \
  }

#define MAKE_TIME_DOUBLE_FIELD(name, type)                               \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    struct timespec spec = match->file->stat.type##time;                 \
    return json_real(spec.tv_sec + 1e-9 * spec.tv_nsec);                 \
  }

/* For each type (e.g. "m"), define fields
 * - mtime: mtime in seconds
 * - mtime_ms: mtime in milliseconds
 * - mtime_us: mtime in microseconds
 * - mtime_ns: mtime in nanoseconds
 * - mtime_f: mtime as a double
 */
#define MAKE_TIME_FIELDS(type) \
  MAKE_INT_FIELD(type##time, type##time.tv_sec) \
  MAKE_TIME_INT_FIELD(type##time_ms, type, 1000) \
  MAKE_TIME_INT_FIELD(type##time_us, type, 1000 * 1000) \
  MAKE_TIME_INT_FIELD(type##time_ns, type, 1000 * 1000 * 1000) \
  MAKE_TIME_DOUBLE_FIELD(type##time_f, type)

MAKE_INT_FIELD(size, size)
MAKE_INT_FIELD(mode, mode)
MAKE_INT_FIELD(uid, uid)
MAKE_INT_FIELD(gid, gid)
MAKE_TIME_FIELDS(a)
MAKE_TIME_FIELDS(m)
MAKE_TIME_FIELDS(c)
MAKE_INT_FIELD(ino, ino)
MAKE_INT_FIELD(dev, dev)
MAKE_INT_FIELD(nlink, nlink)

#define MAKE_TIME_FIELD_DEFS(type) \
  { #type "time", make_##type##time }, \
  { #type "time_ms", make_##type##time_ms }, \
  { #type "time_us", make_##type##time_us }, \
  { #type "time_ns", make_##type##time_ns }, \
  { #type "time_f", make_##type##time_f }

static json_ref make_type_field(const struct watchman_rule_match* match) {
  // Bias towards the more common file types first
  if (S_ISREG(match->file->stat.mode)) {
    return typed_string_to_json("f", W_STRING_UNICODE);
  }
  if (S_ISDIR(match->file->stat.mode)) {
    return typed_string_to_json("d", W_STRING_UNICODE);
  }
  if (S_ISLNK(match->file->stat.mode)) {
    return typed_string_to_json("l", W_STRING_UNICODE);
  }
  if (S_ISBLK(match->file->stat.mode)) {
    return typed_string_to_json("b", W_STRING_UNICODE);
  }
  if (S_ISCHR(match->file->stat.mode)) {
    return typed_string_to_json("c", W_STRING_UNICODE);
  }
  if (S_ISFIFO(match->file->stat.mode)) {
    return typed_string_to_json("p", W_STRING_UNICODE);
  }
  if (S_ISSOCK(match->file->stat.mode)) {
    return typed_string_to_json("s", W_STRING_UNICODE);
  }
#ifdef S_ISDOOR
  if (S_ISDOOR(match->file->stat.mode)) {
    return typed_string_to_json("D", W_STRING_UNICODE);
  }
#endif
  return typed_string_to_json("?", W_STRING_UNICODE);
}

struct w_query_field_renderer {
  w_string name;
  json_ref (*make)(const struct watchman_rule_match* match);
};

// Helper to construct the list of field defs
static std::unordered_map<w_string, w_query_field_renderer> build_defs() {
  struct {
    const char* name;
    json_ref (*make)(const struct watchman_rule_match* match);
  } defs[] = {
      {"name", make_name},
      {"symlink_target", make_symlink},
      {"exists", make_exists},
      {"size", make_size},
      {"mode", make_mode},
      {"uid", make_uid},
      {"gid", make_gid},
      MAKE_TIME_FIELD_DEFS(a),
      MAKE_TIME_FIELD_DEFS(m),
      MAKE_TIME_FIELD_DEFS(c),
      {"ino", make_ino},
      {"dev", make_dev},
      {"nlink", make_nlink},
      {"new", make_new},
      {"oclock", make_oclock},
      {"cclock", make_cclock},
      {"type", make_type_field},
  };
  std::unordered_map<w_string, w_query_field_renderer> map;
  for (auto& def : defs) {
    w_string name(def.name, W_STRING_UNICODE);
    map.emplace(name, w_query_field_renderer{name, def.make});
  }

  return map;
}

// Meyers singleton to avoid SIOF wrt. static constructors in this module
// and the order that w_ctor_fn callbacks are dispatched.
static std::unordered_map<w_string, w_query_field_renderer>& field_defs() {
  static std::unordered_map<w_string, w_query_field_renderer> map(build_defs());
  return map;
}

static w_ctor_fn_type(register_field_capabilities) {
  for (auto& it : field_defs()) {
    char capname[128];
    snprintf(capname, sizeof(capname), "field-%s", it.first.c_str());
    w_capability_register(capname);
  }
}
w_ctor_fn_reg(register_field_capabilities)

json_ref field_list_to_json_name_array(const w_query_field_list& fieldList) {
  auto templ = json_array_of_size(fieldList.size());

  for (auto& f : fieldList) {
    json_array_append_new(templ, w_string_to_json(f->name));
  }

  return templ;
}

json_ref file_result_to_json(
    const w_query_field_list& fieldList,
    const watchman_rule_match& match) {
  if (fieldList.size() == 1) {
    return fieldList.front()->make(&match);
  }
  auto value = json_object_of_size(fieldList.size());

  for (auto& f : fieldList) {
    auto ele = f->make(&match);
    value.set(f->name, std::move(ele));
  }
  return value;
}

bool parse_field_list(
    json_ref field_list,
    w_query_field_list* selected,
    char** errmsg) {
  uint32_t i;

  selected->clear();

  if (!field_list) {
    // Use the default list
    field_list = json_array({typed_string_to_json("name", W_STRING_UNICODE),
                             typed_string_to_json("exists", W_STRING_UNICODE),
                             typed_string_to_json("new", W_STRING_UNICODE),
                             typed_string_to_json("size", W_STRING_UNICODE),
                             typed_string_to_json("mode", W_STRING_UNICODE)});
  }

  if (!json_is_array(field_list)) {
    *errmsg = strdup("field list must be an array of strings");
    return false;
  }

  for (i = 0; i < json_array_size(field_list); i++) {
    auto jname = json_array_get(field_list, i);

    if (!json_is_string(jname)) {
      *errmsg = strdup("field list must be an array of strings");
      return false;
    }

    auto name = json_to_w_string(jname);
    auto& defs = field_defs();
    auto it = defs.find(name);
    if (it == defs.end()) {
      ignore_result(asprintf(errmsg, "unknown field name '%s'", name.c_str()));
      return false;
    }
    selected->push_back(&it->second);
  }

  return true;
}

/* vim:ts=2:sw=2:et:
 */
