/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "Future.h"
using namespace watchman;

static json_ref make_name(const struct watchman_rule_match* match) {
  return w_string_to_json(match->relname);
}

static watchman::Future<json_ref> make_symlink(
    const struct watchman_rule_match* match) {
  return match->file->readLink().then([](Result<w_string>&& result) {
    auto& target = result.value();
    return target ? w_string_to_json(target) : json_null();
  });
}

static watchman::Future<json_ref> make_sha1_hex(
    const struct watchman_rule_match* match) {
  if (!match->file->stat().isFile() || !match->file->exists()) {
    // We return null for items that can't have a content hash
    return makeFuture(json_null());
  }
  return match->file->getContentSha1().then(
      [](Result<FileResult::ContentHash>&& result) {
        try {
          auto& hash = result.value();
          char buf[40];
          static const char* hexDigit = "0123456789abcdef";
          for (size_t i = 0; i < hash.size(); ++i) {
            auto& digit = hash[i];
            buf[(i * 2) + 0] = hexDigit[digit >> 4];
            buf[(i * 2) + 1] = hexDigit[digit & 0xf];
          }
          return w_string_to_json(w_string(buf, sizeof(buf), W_STRING_UNICODE));
        } catch (const std::exception& exc) {
          // We'll report the error wrapped up in an object so that it can be
          // distinguished from a valid hash result.
          return json_object(
              {{"error",
                w_string_to_json(w_string(exc.what(), W_STRING_UNICODE))}});
        }
      });
}

static json_ref make_exists(const struct watchman_rule_match* match) {
  return json_boolean(match->file->exists());
}

static json_ref make_new(const struct watchman_rule_match* match) {
  return json_boolean(match->is_new);
}

#define MAKE_CLOCK_FIELD(name, member)                                   \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    char buf[128];                                                       \
    if (clock_id_string(                                                 \
            match->root_number,                                          \
            match->file->member().ticks,                                 \
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
    return json_integer(match->file->stat().member);                     \
  }

#define MAKE_TIME_INT_FIELD(name, type, scale)                           \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    struct timespec spec = match->file->stat().type##time;               \
    return json_integer(                                                 \
        ((int64_t)spec.tv_sec * scale) +                                 \
        ((int64_t)spec.tv_nsec * scale / WATCHMAN_NSEC_IN_SEC));         \
  }

#define MAKE_TIME_DOUBLE_FIELD(name, type)                               \
  static json_ref make_##name(const struct watchman_rule_match* match) { \
    struct timespec spec = match->file->stat().type##time;               \
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
  { #type "time", make_##type##time, nullptr }, \
  { #type "time_ms", make_##type##time_ms, nullptr }, \
  { #type "time_us", make_##type##time_us, nullptr }, \
  { #type "time_ns", make_##type##time_ns, nullptr }, \
  { #type "time_f", make_##type##time_f, nullptr }

static json_ref make_type_field(const struct watchman_rule_match* match) {
  // Bias towards the more common file types first
  auto& stat = match->file->stat();
  if (stat.isFile()) {
    return typed_string_to_json("f", W_STRING_UNICODE);
  }
  if (stat.isDir()) {
    return typed_string_to_json("d", W_STRING_UNICODE);
  }
  if (stat.isSymlink()) {
    return typed_string_to_json("l", W_STRING_UNICODE);
  }
#ifndef _WIN32
  if (S_ISBLK(stat.mode)) {
    return typed_string_to_json("b", W_STRING_UNICODE);
  }
  if (S_ISCHR(stat.mode)) {
    return typed_string_to_json("c", W_STRING_UNICODE);
  }
  if (S_ISFIFO(stat.mode)) {
    return typed_string_to_json("p", W_STRING_UNICODE);
  }
  if (S_ISSOCK(stat.mode)) {
    return typed_string_to_json("s", W_STRING_UNICODE);
  }
#endif
#ifdef S_ISDOOR
  if (S_ISDOOR(stat.mode)) {
    return typed_string_to_json("D", W_STRING_UNICODE);
  }
#endif
  return typed_string_to_json("?", W_STRING_UNICODE);
}

// Helper to construct the list of field defs
static std::unordered_map<w_string, w_query_field_renderer> build_defs() {
  struct {
    const char* name;
    json_ref (*make)(const struct watchman_rule_match* match);
    watchman::Future<json_ref> (*futureMake)(
        const struct watchman_rule_match* match);
  } defs[] = {
      {"name", make_name, nullptr},
      {"symlink_target", nullptr, make_symlink},
      {"exists", make_exists, nullptr},
      {"size", make_size, nullptr},
      {"mode", make_mode, nullptr},
      {"uid", make_uid, nullptr},
      {"gid", make_gid, nullptr},
      MAKE_TIME_FIELD_DEFS(a),
      MAKE_TIME_FIELD_DEFS(m),
      MAKE_TIME_FIELD_DEFS(c),
      {"ino", make_ino, nullptr},
      {"dev", make_dev, nullptr},
      {"nlink", make_nlink, nullptr},
      {"new", make_new, nullptr},
      {"oclock", make_oclock, nullptr},
      {"cclock", make_cclock, nullptr},
      {"type", make_type_field, nullptr},
      {"content.sha1hex", nullptr, make_sha1_hex},
  };
  std::unordered_map<w_string, w_query_field_renderer> map;
  for (auto& def : defs) {
    w_string name(def.name, W_STRING_UNICODE);
    map.emplace(name, w_query_field_renderer{name, def.make, def.futureMake});
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

watchman::Future<json_ref> file_result_to_json_future(
    const w_query_field_list& fieldList,
    watchman_rule_match&& match) {
  auto matchPtr = std::make_shared<watchman_rule_match>(std::move(match));

  std::vector<watchman::Future<json_ref>> futures;
  for (auto& f : fieldList) {
    if (f->futureMake) {
      futures.emplace_back(f->futureMake(matchPtr.get()));
    } else {
      futures.emplace_back(makeFuture(f->make(matchPtr.get())));
    }
  }

  return collectAll(futures.begin(), futures.end())
      .then([&fieldList, matchPtr](
          Result<std::vector<Result<json_ref>>>&& result) {
        auto& vec = result.value();
        if (fieldList.size() == 1) {
          return vec[0].value();
        }

        auto value = json_object_of_size(vec.size());
        for (size_t i = 0; i < fieldList.size(); ++i) {
          auto& f = fieldList[i];
          value.set(f->name, std::move(vec[i].value()));
        }
        return value;
      });
}

void parse_field_list(json_ref field_list, w_query_field_list* selected) {
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
    throw QueryParseError("field list must be an array of strings");
  }

  for (i = 0; i < json_array_size(field_list); i++) {
    auto jname = json_array_get(field_list, i);

    if (!json_is_string(jname)) {
      throw QueryParseError("field list must be an array of strings");
    }

    auto name = json_to_w_string(jname);
    auto& defs = field_defs();
    auto it = defs.find(name);
    if (it == defs.end()) {
      throw QueryParseError("unknown field name '", name, "'");
    }
    selected->push_back(&it->second);
  }
}

/* vim:ts=2:sw=2:et:
 */
