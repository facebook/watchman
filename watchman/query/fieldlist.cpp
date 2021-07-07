/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman/Errors.h"
#include "watchman/watchman.h"

using namespace watchman;
using folly::Optional;

static Optional<json_ref> make_name(FileResult* file, const w_query_ctx* ctx) {
  return w_string_to_json(ctx->computeWholeName(file));
}

static Optional<json_ref> make_symlink(FileResult* file, const w_query_ctx*) {
  auto target = file->readLink();
  if (!target.has_value()) {
    return folly::none;
  }
  return *target ? w_string_to_json(*target) : json_null();
}

static Optional<json_ref> make_sha1_hex(FileResult* file, const w_query_ctx*) {
  try {
    auto hash = file->getContentSha1();
    if (!hash.has_value()) {
      // Need to load it still
      return folly::none;
    }
    char buf[40];
    static const char* hexDigit = "0123456789abcdef";
    for (size_t i = 0; i < hash->size(); ++i) {
      auto& digit = (*hash)[i];
      buf[(i * 2) + 0] = hexDigit[digit >> 4];
      buf[(i * 2) + 1] = hexDigit[digit & 0xf];
    }
    return w_string_to_json(w_string(buf, sizeof(buf), W_STRING_UNICODE));
  } catch (const std::system_error& exc) {
    auto errcode = exc.code();
    if (errcode == watchman::error_code::no_such_file_or_directory ||
        errcode == watchman::error_code::is_a_directory) {
      // Deleted files, or (currently existing) directories have no hash
      return json_null();
    }
    // We'll report the error wrapped up in an object so that it can be
    // distinguished from a valid hash result.
    return json_object(
        {{"error", w_string_to_json(w_string(exc.what(), W_STRING_UNICODE))}});
  } catch (const std::exception& exc) {
    // We'll report the error wrapped up in an object so that it can be
    // distinguished from a valid hash result.
    return json_object(
        {{"error", w_string_to_json(w_string(exc.what(), W_STRING_UNICODE))}});
  }
}

static Optional<json_ref> make_size(FileResult* file, const w_query_ctx*) {
  auto size = file->size();
  if (!size.has_value()) {
    return folly::none;
  }
  return json_integer(size.value());
}

static Optional<json_ref> make_exists(FileResult* file, const w_query_ctx*) {
  auto exists = file->exists();
  if (!exists.has_value()) {
    return folly::none;
  }
  return json_boolean(exists.value());
}

static Optional<json_ref> make_new(FileResult* file, const w_query_ctx* ctx) {
  bool is_new = false;

  if (!ctx->since.is_timestamp && ctx->since.clock.is_fresh_instance) {
    is_new = true;
  } else {
    auto ctime = file->ctime();
    if (!ctime.has_value()) {
      // Reconsider this one later
      return folly::none;
    }
    if (ctx->since.is_timestamp) {
      is_new = ctx->since.timestamp > ctime->timestamp;
    } else {
      is_new = ctime->ticks > ctx->since.clock.ticks;
    }
  }

  return json_boolean(is_new);
}

#define MAKE_CLOCK_FIELD(name, member)                      \
  static Optional<json_ref> make_##name(                    \
      FileResult* file, const w_query_ctx* ctx) {           \
    char buf[128];                                          \
    auto clock = file->member();                            \
    if (!clock.has_value()) {                               \
      /* need to load data */                               \
      return folly::none;                                   \
    }                                                       \
    if (clock_id_string(                                    \
            ctx->clockAtStartOfQuery.position().rootNumber, \
            clock->ticks,                                   \
            buf,                                            \
            sizeof(buf))) {                                 \
      return typed_string_to_json(buf, W_STRING_UNICODE);   \
    }                                                       \
    return json_null();                                     \
  }
MAKE_CLOCK_FIELD(cclock, ctime)
MAKE_CLOCK_FIELD(oclock, otime)

// Note: our JSON library supports 64-bit integers, but this may
// pose a compatibility issue for others.  We'll see if anyone
// runs into an issue and deal with it then...
static_assert(
    sizeof(json_int_t) >= sizeof(time_t),
    "json_int_t isn't large enough to hold a time_t");

#define MAKE_INT_FIELD(name, member)          \
  static Optional<json_ref> make_##name(      \
      FileResult* file, const w_query_ctx*) { \
    auto stat = file->stat();                 \
    if (!stat.has_value()) {                  \
      /* need to load data */                 \
      return folly::none;                     \
    }                                         \
    return json_integer(stat->member);        \
  }

#define MAKE_TIME_INT_FIELD(name, member, scale)                  \
  static Optional<json_ref> make_##name(                          \
      FileResult* file, const w_query_ctx*) {                     \
    auto spec = file->member();                                   \
    if (!spec.has_value()) {                                      \
      /* need to load data */                                     \
      return folly::none;                                         \
    }                                                             \
    return json_integer(                                          \
        ((int64_t)spec->tv_sec * scale) +                         \
        ((int64_t)spec->tv_nsec * scale / WATCHMAN_NSEC_IN_SEC)); \
  }

#define MAKE_TIME_DOUBLE_FIELD(name, member)               \
  static Optional<json_ref> make_##name(                   \
      FileResult* file, const w_query_ctx*) {              \
    auto spec = file->member();                            \
    if (!spec.has_value()) {                               \
      /* need to load data */                              \
      return folly::none;                                  \
    }                                                      \
    return json_real(spec->tv_sec + 1e-9 * spec->tv_nsec); \
  }

/* For each type (e.g. "m"), define fields
 * - mtime: mtime in seconds
 * - mtime_ms: mtime in milliseconds
 * - mtime_us: mtime in microseconds
 * - mtime_ns: mtime in nanoseconds
 * - mtime_f: mtime as a double
 */
#define MAKE_TIME_FIELDS(type, member)                           \
  MAKE_TIME_INT_FIELD(type##time, member, 1)                     \
  MAKE_TIME_INT_FIELD(type##time_ms, member, 1000)               \
  MAKE_TIME_INT_FIELD(type##time_us, member, 1000 * 1000)        \
  MAKE_TIME_INT_FIELD(type##time_ns, member, 1000 * 1000 * 1000) \
  MAKE_TIME_DOUBLE_FIELD(type##time_f, member)

MAKE_INT_FIELD(mode, mode)
MAKE_INT_FIELD(uid, uid)
MAKE_INT_FIELD(gid, gid)
MAKE_TIME_FIELDS(a, accessedTime)
MAKE_TIME_FIELDS(m, modifiedTime)
MAKE_TIME_FIELDS(c, changedTime)
MAKE_INT_FIELD(ino, ino)
MAKE_INT_FIELD(dev, dev)
MAKE_INT_FIELD(nlink, nlink)

// clang-format off
#define MAKE_TIME_FIELD_DEFS(type) \
  { #type "time", make_##type##time}, \
  { #type "time_ms", make_##type##time_ms},\
  { #type "time_us", make_##type##time_us}, \
  { #type "time_ns", make_##type##time_ns}, \
  { #type "time_f", make_##type##time_f}
// clang-format on

static Optional<json_ref> make_type_field(
    FileResult* file,
    const w_query_ctx*) {
  auto dtype = file->dtype();
  if (dtype.has_value()) {
    switch (*dtype) {
      case DType::Regular:
        return typed_string_to_json("f", W_STRING_UNICODE);
      case DType::Dir:
        return typed_string_to_json("d", W_STRING_UNICODE);
      case DType::Symlink:
        return typed_string_to_json("l", W_STRING_UNICODE);
      case DType::Block:
        return typed_string_to_json("b", W_STRING_UNICODE);
      case DType::Char:
        return typed_string_to_json("c", W_STRING_UNICODE);
      case DType::Fifo:
        return typed_string_to_json("p", W_STRING_UNICODE);
      case DType::Socket:
        return typed_string_to_json("s", W_STRING_UNICODE);
      case DType::Whiteout:
        // Whiteout shouldn't generally be visible to userspace,
        // and we don't have a defined letter code for it, so
        // treat it as "who knows!?"
        return typed_string_to_json("?", W_STRING_UNICODE);
      case DType::Unknown:
      default:
          // Not enough info; fall through and use the full stat data
          ;
    }
  }

  // Bias towards the more common file types first
  auto optionalStat = file->stat();
  if (!optionalStat.has_value()) {
    return folly::none;
  }

  auto stat = optionalStat.value();
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
    Optional<json_ref> (*make)(FileResult* file, const w_query_ctx* ctx);
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
      {"content.sha1hex", make_sha1_hex},
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

json_ref field_list_to_json_name_array(const w_query_field_list& fieldList) {
  auto templ = json_array_of_size(fieldList.size());

  for (auto& f : fieldList) {
    json_array_append_new(templ, w_string_to_json(f->name));
  }

  return templ;
}

Optional<json_ref> file_result_to_json(
    const w_query_field_list& fieldList,
    const std::unique_ptr<FileResult>& file,
    const w_query_ctx* ctx) {
  if (fieldList.size() == 1) {
    return fieldList.front()->make(file.get(), ctx);
  }
  auto value = json_object_of_size(fieldList.size());

  for (auto& f : fieldList) {
    auto ele = f->make(file.get(), ctx);
    if (!ele.has_value()) {
      // Need data to be loaded
      return folly::none;
    }
    value.set(f->name, std::move(ele.value()));
  }
  return value;
}

void parse_field_list(json_ref field_list, w_query_field_list* selected) {
  uint32_t i;

  selected->clear();

  if (!field_list) {
    // Use the default list
    field_list = json_array(
        {typed_string_to_json("name", W_STRING_UNICODE),
         typed_string_to_json("exists", W_STRING_UNICODE),
         typed_string_to_json("new", W_STRING_UNICODE),
         typed_string_to_json("size", W_STRING_UNICODE),
         typed_string_to_json("mode", W_STRING_UNICODE)});
  }

  if (!field_list.isArray()) {
    throw QueryParseError("field list must be an array of strings");
  }

  for (i = 0; i < json_array_size(field_list); i++) {
    auto jname = json_array_get(field_list, i);

    if (!jname.isString()) {
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

static w_ctor_fn_type(register_field_capabilities) {
  for (auto& it : field_defs()) {
    char capname[128];
    snprintf(capname, sizeof(capname), "field-%s", it.first.c_str());
    capability_register(capname);
  }
}

// This is at the bottom because it confuses clang-format for things that follow
w_ctor_fn_reg(register_field_capabilities)
