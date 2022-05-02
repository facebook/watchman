/* Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "jansson.h"
#include "jansson_private.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "utf.h"
#include "watchman/watchman_string.h"

namespace {
const char* getTypeName(json_type t) {
  switch (t) {
    case JSON_OBJECT:
      return "object";
    case JSON_ARRAY:
      return "array";
    case JSON_STRING:
      return "string";
    case JSON_INTEGER:
      return "integer";
    case JSON_REAL:
      return "real";
    case JSON_TRUE:
      return "true";
    case JSON_FALSE:
      return "false";
    case JSON_NULL:
      return "null";
  }
  return "<unknown>";
}
} // namespace

json_ref::json_ref() : ref_(nullptr) {}
json_ref::json_ref(std::nullptr_t) : ref_(nullptr) {}

json_ref::json_ref(json_t* ref, bool addRef) : ref_(ref) {
  if (addRef && ref_) {
    incref(ref_);
  }
}

json_ref::~json_ref() {
  reset();
}

void json_ref::reset(json_t* ref) {
  if (ref_ == ref) {
    return;
  }

  if (ref_) {
    decref(ref_);
  }

  ref_ = ref;

  if (ref_) {
    incref(ref_);
  }
}

json_ref::json_ref(const json_ref& other) : ref_(nullptr) {
  reset(other.ref_);
}

json_ref& json_ref::operator=(const json_ref& other) {
  reset(other.ref_);
  return *this;
}

json_ref::json_ref(json_ref&& other) noexcept : ref_(other.ref_) {
  other.ref_ = nullptr;
}

json_ref& json_ref::operator=(json_ref&& other) noexcept {
  reset();
  std::swap(ref_, other.ref_);
  return *this;
}

json_t::json_t(json_type type) : type(type), refcount(1) {}

json_t::json_t(json_type type, json_t::SingletonHack&&)
    : type(type), refcount(-1) {}

const w_string& json_ref::asString() const {
  if (!*this || !isString()) {
    throw std::domain_error(
        fmt::format("json_ref expected string, got {}", getTypeName(type())));
  }
  return json_to_string(ref_)->value;
}

std::optional<w_string> json_ref::asOptionalString() const {
  if (!*this || !isString()) {
    return std::nullopt;
  }
  return json_to_string(ref_)->value;
}

const char* json_ref::asCString() const {
  return asString().c_str();
}

bool json_ref::asBool() const {
  switch (type()) {
    case JSON_TRUE:
      return true;
    case JSON_FALSE:
      return false;
    default:
      throw std::domain_error(
          fmt::format("asBool called on non-boolean: {}", getTypeName(type())));
  }
}

/*** object ***/

const std::unordered_map<w_string, json_ref>& json_ref::object() const {
  if (!*this || type() != JSON_OBJECT) {
    throw std::domain_error("json_ref::object() called for non-object");
  }
  return json_to_object(ref_)->map;
}

std::unordered_map<w_string, json_ref>& json_ref::object() {
  if (!*this || type() != JSON_OBJECT) {
    throw std::domain_error("json_ref::object() called for non-object");
  }
  return json_to_object(ref_)->map;
}

json_object_t::json_object_t(size_t sizeHint) : json_t(JSON_OBJECT) {
  map.reserve(sizeHint);
}

json_ref json_object_of_size(size_t size) {
  return json_ref(new json_object_t(size), false);
}

json_ref json_object(
    std::initializer_list<std::pair<const char*, json_ref>> values) {
  auto object = json_object_of_size(values.size());
  auto& map = json_to_object(object)->map;
  for (auto& it : values) {
    map.emplace(w_string(it.first, W_STRING_UNICODE), it.second);
  }

  return object;
}

json_ref json_object() {
  return json_object_of_size(0);
}

size_t json_object_size(const json_ref& json) {
  if (!json || !json.isObject()) {
    return 0;
  }

  return json_to_object(json)->map.size();
}

typename std::unordered_map<w_string, json_ref>::iterator
json_object_t::findCString(const char* key) {
  w_string key_string(key, W_STRING_BYTE);
  return map.find(key_string);
}

const json_ref& json_ref::get(const char* key) const {
  if (!*this || type() != JSON_OBJECT) {
    throw std::domain_error("json_ref::get called on a non object type");
  }
  auto object = json_to_object(ref_);
  auto it = object->findCString(key);
  if (it == object->map.end()) {
    throw std::range_error(
        std::string("key '") + key + "' is not present in this json object");
  }
  return it->second;
}

json_ref json_ref::get_default(const char* key, json_ref defval) const {
  if (!*this || type() != JSON_OBJECT) {
    return defval;
  }
  auto object = json_to_object(ref_);
  auto it = object->findCString(key);
  if (it == object->map.end()) {
    return defval;
  }
  return it->second;
}

json_ref json_object_get(const json_ref& json, const char* key) {
  if (!json || !json.isObject()) {
    return nullptr;
  }

  auto* object = json_to_object(json);
  auto it = object->findCString(key);
  if (it == object->map.end()) {
    return nullptr;
  }
  return it->second;
}

int json_object_set_new_nocheck(
    const json_ref& json,
    const char* key,
    json_ref&& value) {
  if (!json || !json.isObject()) {
    return -1;
  }

  if (!value)
    return -1;

  if (!key || json.get() == value.get()) {
    return -1;
  }
  auto* object = json_to_object(json);

  w_string key_string(key);
  object->map[key_string] = std::move(value);
  return 0;
}

void json_ref::set(const w_string& key, json_ref&& val) {
  json_to_object(ref_)->map[key] = std::move(val);
}

void json_ref::set(const char* key, json_ref&& val) {
#if 0 // circular build dep
  w_assert(key != nullptr, "json_ref::set called with NULL key");
  w_assert(ref_ != nullptr, "json_ref::set called NULL object");
  w_assert(val != ref_, "json_ref::set cannot create cycle");
  w_assert(json_is_object(ref_), "json_ref::set called for non object type");
#endif

  w_string key_string(key);
  json_to_object(ref_)->map[key_string] = std::move(val);
}

int json_object_set_new(const json_ref& json, const char* key, json_ref&& value) {
  if (!key || !utf8_check_string(key, -1)) {
    return -1;
  }

  return json_object_set_new_nocheck(json, key, std::move(value));
}

int json_object_update(const json_ref& src, const json_ref& target) {
  if (!src || !target || !src.isObject() || !target.isObject())
    return -1;

  auto target_obj = json_to_object(target);
  for (auto& it : json_to_object(src)->map) {
    target_obj->map[it.first] = it.second;
  }

  return 0;
}

static int json_object_equal(const json_ref& object1, const json_ref& object2) {
  if (json_object_size(object1) != json_object_size(object2))
    return 0;

  auto target_obj = json_to_object(object2);
  for (auto& it : json_to_object(object1)->map) {
    auto other_it = target_obj->map.find(it.first);

    if (other_it == target_obj->map.end()) {
      return 0;
    }

    if (!json_equal(it.second, other_it->second)) {
      return 0;
    }
  }

  return 1;
}

static json_ref json_object_deep_copy(const json_ref& object) {
  json_ref result = json_object();
  if (!result)
    return nullptr;

  auto target_obj = json_to_object(result);
  for (auto& it : json_to_object(object)->map) {
    target_obj->map[it.first] = json_deep_copy(it.second);
  }

  return result;
}

/*** array ***/

json_array_t::json_array_t(size_t sizeHint) : json_t(JSON_ARRAY) {
  table.reserve(std::max(sizeHint, size_t(8)));
}

json_array_t::json_array_t(std::initializer_list<json_ref> values)
    : json_t(JSON_ARRAY), table(values) {}

const std::vector<json_ref>& json_ref::array() const {
  if (!*this || !isArray()) {
    throw std::domain_error("json_ref::array() called for non-array");
  }
  return json_to_array(ref_)->table;
}

std::vector<json_ref>& json_ref::array() {
  if (!*this || !isArray()) {
    throw std::domain_error("json_ref::array() called for non-array");
  }
  return json_to_array(ref_)->table;
}

json_ref json_array_of_size(size_t nelems) {
  return json_ref(new json_array_t(nelems), false);
}

json_ref json_array() {
  return json_array_of_size(8);
}

json_ref json_array(std::initializer_list<json_ref> values) {
  return json_ref(new json_array_t(std::move(values)), false);
}

int json_array_set_template(const json_ref& json, const json_ref& templ) {
  return json_array_set_template_new(json, json_ref(templ));
}

int json_array_set_template_new(const json_ref& json, json_ref&& templ) {
  if (!json || !json.isArray()) {
    return 0;
  }
  json_to_array(json)->templ = std::move(templ);
  return 1;
}

json_ref json_array_get_template(const json_ref& array) {
  if (!array || !array.isArray()) {
    return nullptr;
  }
  return json_to_array(array)->templ;
}

size_t json_array_size(const json_ref& json) {
  if (!json || !json.isArray()) {
    return 0;
  }

  return json_to_array(json)->table.size();
}

json_ref json_array_get(const json_ref& json, size_t index) {
  if (!json || !json.isArray()) {
    return nullptr;
  }
  auto array = json_to_array(json);

  if (index >= array->table.size()) {
    return nullptr;
  }

  return array->table[index];
}

int json_array_set_new(const json_ref& json, size_t index, json_ref&& value) {
  json_array_t* array;

  if (!value)
    return -1;

  if (!json || !json.isArray() || json.get() == value.get()) {
    return -1;
  }
  array = json_to_array(json);

  if (index >= array->table.size()) {
    return -1;
  }

  array->table[index] = std::move(value);

  return 0;
}

int json_array_append(const json_ref& json, json_ref value) {
  if (!value)
    return -1;

  if (!json || !json.isArray() || json.get() == value.get()) {
    return -1;
  }
  json_to_array(json)->table.push_back(std::move(value));
  return 0;
}

int json_array_insert_new(
    const json_ref& json,
    size_t index,
    json_ref&& value) {
  if (!value)
    return -1;

  if (!json || !json.isArray() || json.get() == value.get()) {
    return -1;
  }
  auto array = json_to_array(json);
  if (index > array->table.size()) {
    return -1;
  }

  auto it = array->table.begin();
  std::advance(it, index);

  array->table.insert(it, std::move(value));
  return 0;
}

int json_array_remove(const json_ref& json, size_t index) {
  json_array_t* array;

  if (!json || !json.isArray()) {
    return -1;
  }
  array = json_to_array(json);

  if (index > array->table.size()) {
    return -1;
  }

  auto it = array->table.begin();
  std::advance(it, index);

  array->table.erase(it);
  return 0;
}

static int json_array_equal(const json_ref& array1, const json_ref& array2) {
  size_t i, size;

  size = json_array_size(array1);
  if (size != json_array_size(array2))
    return 0;

  for (i = 0; i < size; i++) {
    auto value1 = json_array_get(array1, i);
    auto value2 = json_array_get(array2, i);

    if (!json_equal(value1, value2))
      return 0;
  }

  return 1;
}

static json_ref json_array_deep_copy(const json_ref& array) {
  auto result = json_array();
  if (!result)
    return nullptr;

  for (auto& elem : array.array())
    json_array_append(result, json_deep_copy(elem));

  return result;
}

/*** string ***/

json_string_t::json_string_t(w_string str)
    : json_t(JSON_STRING), value(std::move(str)) {}

json_ref w_string_to_json(w_string str) {
  if (!str) {
    return json_null();
  }

  return json_ref(new json_string_t(str), false);
}

const char* json_string_value(const json_ref& json) {
  if (!json || !json.isString()) {
    return nullptr;
  }

  return json_to_string(json)->value.c_str();
}

const w_string& json_to_w_string(const json_ref& json) {
  if (!json || !json.isString()) {
    throw std::runtime_error("expected json string object");
  }

  return json_to_string(json)->value;
}

static int json_string_equal(const json_ref& string1, const json_ref& string2) {
  return json_to_string(string1)->value == json_to_string(string2)->value;
}

/*** integer ***/

json_integer_t::json_integer_t(json_int_t value)
    : json_t(JSON_INTEGER), value(value) {}

json_ref json_integer(json_int_t value) {
  return json_ref(new json_integer_t(value), false);
}

json_int_t json_integer_value(const json_ref& json) {
  if (!json || !json.isInt()) {
    return 0;
  }
  return json_to_integer(json)->value;
}

json_int_t json_ref::asInt() const {
  return json_integer_value(ref_);
}

static int json_integer_equal(const json_ref& integer1, const json_ref& integer2) {
  return json_integer_value(integer1) == json_integer_value(integer2);
}

/*** real ***/

json_real_t::json_real_t(double value) : json_t(JSON_REAL), value(value) {}

json_ref json_real(double value) {
  if (std::isnan(value) || std::isinf(value)) {
    return nullptr;
  }
  return json_ref(new json_real_t(value), false);
}

double json_real_value(const json_ref& json) {
  if (!json || !json.isDouble()) {
    return 0;
  }

  return json_to_real(json)->value;
}

static int json_real_equal(const json_ref& real1, const json_ref& real2) {
  return json_real_value(real1) == json_real_value(real2);
}

/*** number ***/

double json_number_value(const json_ref& json) {
  if (!json) {
    return 0.0;
  }
  if (json.isInt())
    return (double)json_integer_value(json);
  else if (json.isDouble())
    return json_real_value(json);
  else
    return 0.0;
}

/*** simple values ***/

json_ref json_true() {
  static json_t the_true{JSON_TRUE, json_t::SingletonHack()};
  return &the_true;
}

json_ref json_false() {
  static json_t the_false{JSON_FALSE, json_t::SingletonHack()};
  return &the_false;
}

json_ref json_null() {
  static json_t the_null{JSON_NULL, json_t::SingletonHack()};
  return &the_null;
}

/*** deletion ***/

void json_ref::json_delete(json_t* json) {
  switch (json->type) {
    case JSON_OBJECT:
      delete (json_object_t*)json;
      break;
    case JSON_ARRAY:
      delete (json_array_t*)json;
      break;
    case JSON_STRING:
      delete (json_string_t*)json;
      break;
    case JSON_INTEGER:
      delete (json_integer_t*)json;
      break;
    case JSON_REAL:
      delete (json_real_t*)json;
      break;
    case JSON_TRUE:
    case JSON_FALSE:
    case JSON_NULL:
      break;
  }
}

/*** equality ***/

int json_equal(const json_ref& json1, const json_ref& json2) {
  if (!json1 || !json2)
    return 0;

  if (json1.type() != json2.type())
    return 0;

  /* this covers true, false and null as they are singletons */
  if (json1.get() == json2.get())
    return 1;

  if (json1.isObject())
    return json_object_equal(json1, json2);

  if (json1.isArray())
    return json_array_equal(json1, json2);

  if (json1.isString())
    return json_string_equal(json1, json2);

  if (json1.isInt())
    return json_integer_equal(json1, json2);

  if (json1.isDouble())
    return json_real_equal(json1, json2);

  return 0;
}

/*** copying ***/

json_ref json_deep_copy(const json_ref& json) {
  if (!json)
    return nullptr;

  if (json.isObject())
    return json_object_deep_copy(json);

  if (json.isArray())
    return json_array_deep_copy(json);

  // For the rest of the types, the values are immutable, so just increment the
  // reference count.

  return json;
}
