/* Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef JANSSON_H
#define JANSSON_H

#include "watchman_string.h" // Needed for w_string_t

#include <stdarg.h>
#include <stdio.h>
#include <cstdlib> /* for size_t */
#include <unordered_map>

#include <atomic>
#include <vector>
#include "jansson_config.h"
#include "utf.h"

/* version */

#define JANSSON_MAJOR_VERSION  2
#define JANSSON_MINOR_VERSION  4
#define JANSSON_MICRO_VERSION  0

/* Micro version is omitted if it's 0 */
#define JANSSON_VERSION  "2.4"

/* Version as a 3-byte hex number, e.g. 0x010201 == 1.2.1. Use this
   for numeric comparisons, e.g. #if JANSSON_VERSION_HEX >= ... */
#define JANSSON_VERSION_HEX  ((JANSSON_MAJOR_VERSION << 16) |   \
                              (JANSSON_MINOR_VERSION << 8)  |   \
                              (JANSSON_MICRO_VERSION << 0))


/* types */

typedef enum {
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_INTEGER,
    JSON_REAL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
} json_type;

struct json_t {
    json_type type;
    std::atomic<size_t> refcount;

    explicit json_t(json_type type);

    struct SingletonHack {};
    // true, false, null are never heap allocated, always
    // reference a global singleton value with a bogus refcount
    json_t(json_type type, SingletonHack&&);
};

class json_ref {
  json_t* ref_;

  static inline json_t* incref(json_t* json) {
    if (json && json->refcount != (size_t)-1) {
      ++json->refcount;
    }
    return json;
  }

  static inline void decref(json_t* json) {
    if (json && json->refcount != (size_t)-1 && --json->refcount == 0) {
      json_delete(json);
    }
  }
  static void json_delete(json_t* json);

 public:
  json_ref();
  /* implicit */ json_ref(json_t* ref, bool addRef = true);
  /* implicit */ json_ref(std::nullptr_t);

  ~json_ref();
  void reset(json_t* ref = nullptr);

  json_ref(const json_ref& other);
  json_ref& operator=(const json_ref& other);

  json_ref(json_ref&& other) noexcept;
  json_ref& operator=(json_ref&& other) noexcept;

  /* implicit */ operator json_t*() const {
    return ref_;
  }

  /* implicit */ operator bool() const {
    return ref_ != nullptr;
  }

  /** Returns the value associated with key in a json object.
   * Returns defval if this json value is not an object or
   * if the key was not found. */
  json_ref get_default(const char* key, json_ref defval = nullptr) const;

  /** Returns the vaule associated with key in a json object.
   * Throws domain_error if this is not a json object or
   * a range_error if the key is not present. */
  const json_ref& get(const char* key) const;

  /** Set key = value */
  void set(const char* key, json_ref&& val);
  void set(const w_string& key, json_ref&& val);

  /** Set a list of key/value pairs */
  inline void set(
      std::initializer_list<std::pair<const char*, json_ref&&>> pairs) {
    for (auto& p : pairs) {
      set(p.first, std::move(p.second));
    }
  }

  /** Returns a reference to the underlying array.
   * Throws domain_error if this is not an array.
   * This is useful both for iterating the array contents
   * and for returning the size of the array. */
  const std::vector<json_ref>& array() const;
  std::vector<json_ref>& array();

  /** Returns a reference to the underlying map object.
   * Throws domain_error if this is not an object.
   * This is useful for iterating over the object contents, etc.
   */
  const std::unordered_map<w_string, json_ref>& object() const;
  std::unordered_map<w_string, json_ref>& object();

  /** Returns a reference to the array value at the specified index.
   * Throws out_of_range or domain_error if the index is bad or if
   * this is not an array */
  inline const json_ref& at(std::size_t idx) const {
    return array().at(idx);
  }
};

#if JSON_INTEGER_IS_LONG_LONG
#ifdef _WIN32
#define JSON_INTEGER_FORMAT "I64d"
#else
#define JSON_INTEGER_FORMAT "lld"
#endif
typedef long long json_int_t;
#else
#define JSON_INTEGER_FORMAT "ld"
typedef long json_int_t;
#endif /* JSON_INTEGER_IS_LONG_LONG */

inline json_type json_typeof(const json_t* json) {
  return json->type;
}
#define json_is_object(json)   (json && json_typeof(json) == JSON_OBJECT)
#define json_is_array(json)    (json && json_typeof(json) == JSON_ARRAY)
#define json_is_string(json)   (json && json_typeof(json) == JSON_STRING)
#define json_is_integer(json)  (json && json_typeof(json) == JSON_INTEGER)
#define json_is_real(json)     (json && json_typeof(json) == JSON_REAL)
#define json_is_number(json)   (json_is_integer(json) || json_is_real(json))
#define json_is_true(json)     (json && json_typeof(json) == JSON_TRUE)
#define json_is_false(json)    (json && json_typeof(json) == JSON_FALSE)
#define json_is_boolean(json)  (json_is_true(json) || json_is_false(json))
#define json_is_null(json)     (json && json_typeof(json) == JSON_NULL)

/* construction, destruction, reference counting */

json_ref json_object(void);
json_ref json_object(
    std::initializer_list<std::pair<const char*, json_ref>> values);
json_ref json_object_of_size(size_t nelems);
json_ref json_array(void);
json_ref json_array_of_size(size_t nelems);
json_ref json_array(std::initializer_list<json_ref> values);
json_ref w_string_to_json(const w_string& str);

template <typename... Args>
json_ref typed_string_to_json(Args&&... args) {
  return w_string_to_json(w_string(std::forward<Args>(args)...));
}

const w_string& json_to_w_string(const json_t* json);
json_ref json_integer(json_int_t value);
json_ref json_real(double value);
json_ref json_true(void);
json_ref json_false(void);
#define json_boolean(val)      ((val) ? json_true() : json_false())
json_ref json_null(void);

/* error reporting */

#define JSON_ERROR_TEXT_LENGTH    160
#define JSON_ERROR_SOURCE_LENGTH   80

struct json_error_t {
    int line;
    int column;
    int position;
    char source[JSON_ERROR_SOURCE_LENGTH];
    char text[JSON_ERROR_TEXT_LENGTH];
};

/* getters, setters, manipulation */

size_t json_object_size(const json_t *object);
json_t *json_object_get(const json_t *object, const char *key);
int json_object_set_new(json_t* object, const char* key, json_ref&& value);
int json_object_set_new_nocheck(
    json_t* object,
    const char* key,
    json_ref&& value);
int json_object_del(json_t *object, const char *key);
int json_object_clear(json_t *object);
int json_object_update(const json_t* src, json_t* target);
int json_object_update_existing(const json_t* src, json_t* target);
int json_object_update_missing(const json_t* src, json_t* target);

static JSON_INLINE
int json_object_set(json_t *object, const char *key, json_t *value)
{
  return json_object_set_new(object, key, json_ref(value));
}

static JSON_INLINE
int json_object_set_nocheck(json_t *object, const char *key, json_t *value)
{
  return json_object_set_new_nocheck(object, key, json_ref(value));
}

size_t json_array_size(const json_t *array);
json_ref json_array_get(const json_t* array, size_t index);
int json_array_set_new(json_t* array, size_t index, json_ref&& value);
int json_array_append_new(json_t* array, json_ref&& value);
int json_array_insert_new(json_t* array, size_t index, json_ref&& value);
int json_array_remove(json_t *array, size_t index);
int json_array_clear(json_t *array);
int json_array_extend(json_t *array, json_t *other);
int json_array_set_template(json_t *array, json_t *templ);
int json_array_set_template_new(json_t* json, json_ref&& templ);
json_t *json_array_get_template(const json_t *array);

static JSON_INLINE
int json_array_set(json_t *array, size_t index, json_t *value)
{
  return json_array_set_new(array, index, json_ref(value));
}

static JSON_INLINE
int json_array_append(json_t *array, json_t *value)
{
  return json_array_append_new(array, json_ref(value));
}

static JSON_INLINE
int json_array_insert(json_t *array, size_t index, json_t *value)
{
  return json_array_insert_new(array, index, json_ref(value));
}

const char *json_string_value(const json_t *string);
json_int_t json_integer_value(const json_t *integer);
double json_real_value(const json_t *real);
double json_number_value(const json_t *json);

int json_integer_set(json_t *integer, json_int_t value);
int json_real_set(json_t *real, double value);


#define JSON_VALIDATE_ONLY  0x1
#define JSON_STRICT         0x2

int json_unpack(json_t *root, const char *fmt, ...);
int json_unpack_ex(json_t *root, json_error_t *error, size_t flags, const char *fmt, ...);
int json_vunpack_ex(json_t *root, json_error_t *error, size_t flags, const char *fmt, va_list ap);


/* equality */

int json_equal(json_t *value1, json_t *value2);


/* copying */

json_ref json_copy(const json_t* value);
json_ref json_deep_copy(const json_t* value);

/* decoding */

#define JSON_REJECT_DUPLICATES 0x1
#define JSON_DISABLE_EOF_CHECK 0x2
#define JSON_DECODE_ANY        0x4

typedef size_t (*json_load_callback_t)(void *buffer, size_t buflen, void *data);

json_ref json_loads(const char* input, size_t flags, json_error_t* error);
json_ref json_loadb(
    const char* buffer,
    size_t buflen,
    size_t flags,
    json_error_t* error);
json_ref json_loadf(FILE* input, size_t flags, json_error_t* error);
json_ref json_load_file(const char* path, size_t flags, json_error_t* error);
json_ref json_load_callback(
    json_load_callback_t callback,
    void* data,
    size_t flags,
    json_error_t* error);

/* encoding */

#define JSON_INDENT(n)      (n & 0x1F)
#define JSON_COMPACT        0x20
#define JSON_ENSURE_ASCII   0x40
#define JSON_SORT_KEYS      0x80
#define JSON_ENCODE_ANY     0x200
#define JSON_ESCAPE_SLASH   0x400

typedef int (*json_dump_callback_t)(const char *buffer, size_t size, void *data);

char *json_dumps(const json_t *json, size_t flags);
int json_dumpf(const json_t *json, FILE *output, size_t flags);
int json_dump_file(const json_t *json, const char *path, size_t flags);
int json_dump_callback(const json_t *json, json_dump_callback_t callback, void *data, size_t flags);

/* custom memory allocation */

typedef void *(*json_malloc_t)(size_t);
typedef void (*json_free_t)(void *);

void json_set_alloc_funcs(json_malloc_t malloc_fn, json_free_t free_fn);

#endif
