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

static struct w_query_field_renderer {
  const char *name;
  json_ref (*make)(const struct watchman_rule_match* match);
} field_defs[] = {
  { "name", make_name },
  { "symlink_target", make_symlink },
  { "exists", make_exists },
  { "size", make_size },
  { "mode", make_mode },
  { "uid", make_uid },
  { "gid", make_gid },
  MAKE_TIME_FIELD_DEFS(a),
  MAKE_TIME_FIELD_DEFS(m),
  MAKE_TIME_FIELD_DEFS(c),
  { "ino", make_ino },
  { "dev", make_dev },
  { "nlink", make_nlink },
  { "new", make_new },
  { "oclock", make_oclock },
  { "cclock", make_cclock },
  { "type", make_type_field },
  { NULL, NULL }
};

static w_ctor_fn_type(register_field_capabilities) {
  int f;

  for (f = 0; field_defs[f].name; f++) {
    char capname[128];
    snprintf(capname, sizeof(capname), "field-%s", field_defs[f].name);
    w_capability_register(capname);
  }
}
w_ctor_fn_reg(register_field_capabilities)

    json_ref w_query_results_to_json(
        struct w_query_field_list* field_list,
        uint32_t num_results,
        const std::deque<watchman_rule_match>& results) {
  auto file_list = json_array_of_size(num_results);
  uint32_t i, f;

  // build a template for the serializer
  if (num_results && field_list->num_fields > 1) {
    auto templ = json_array_of_size(field_list->num_fields);

    for (f = 0; f < field_list->num_fields; f++) {
      json_array_append_new(templ,
          typed_string_to_json(field_list->fields[f]->name,
          W_STRING_BYTE));
    }

    json_array_set_template_new(file_list, std::move(templ));
  }

  for (i = 0; i < num_results; i++) {
    json_ref value, ele;

    if (field_list->num_fields == 1) {
      value = field_list->fields[0]->make(&results[i]);
    } else {
      value = json_object_of_size(field_list->num_fields);

      for (f = 0; f < field_list->num_fields; f++) {
        ele = field_list->fields[f]->make(&results[i]);
        value.set(field_list->fields[f]->name, std::move(ele));
      }
    }
    json_array_append_new(file_list, std::move(value));
  }
  return file_list;
}

bool parse_field_list(
    json_ref field_list,
    struct w_query_field_list* selected,
    char** errmsg) {
  uint32_t i, f;

  memset(selected, 0, sizeof(*selected));

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
    json_t *jname = json_array_get(field_list, i);
    const char *name;
    bool found = false;

    if (!json_is_string(jname)) {
      *errmsg = strdup("field list must be an array of strings");
      return false;
    }

    name = json_string_value(jname);

    for (f = 0; field_defs[f].name; f++) {
      if (!strcmp(name, field_defs[f].name)) {
        found = true;
        selected->fields[selected->num_fields++] = &field_defs[f];
        break;
      }
    }

    if (!found) {
      ignore_result(asprintf(errmsg, "unknown field name '%s'", name));
      return false;
    }
  }

  return true;
}

/* vim:ts=2:sw=2:et:
 */
