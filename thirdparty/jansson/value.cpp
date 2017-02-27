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
#include "watchman_log.h"
#include "watchman_string.h"

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

/*** object ***/

const std::unordered_map<w_string, json_ref>& json_ref::object() const {
  if (!json_is_object(ref_)) {
    throw std::domain_error("json_ref::object() called for non-object");
  }
  return json_to_object(ref_)->map;
}

std::unordered_map<w_string, json_ref>& json_ref::object() {
  if (!json_is_object(ref_)) {
    throw std::domain_error("json_ref::object() called for non-object");
  }
  return json_to_object(ref_)->map;
}

json_object_t::json_object_t(size_t sizeHint) : json(JSON_OBJECT) {
  map.reserve(sizeHint);
}

json_ref json_object_of_size(size_t size) {
  auto object = new json_object_t(size);
  return json_ref(&object->json, false);
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

json_ref json_object(void) {
  return json_object_of_size(0);
}

size_t json_object_size(const json_t *json)
{
    json_object_t *object;

    if(!json_is_object(json))
        return 0;

    object = json_to_object(json);
    return object->map.size();
}

typename std::unordered_map<w_string, json_ref>::iterator
json_object_t::findCString(const char* key) {
  // Avoid making a copy of the string for this lookup
  w_string_t key_string;
  w_string_new_len_typed_stack(
      &key_string, key, strlen_uint32(key), W_STRING_BYTE);
  return map.find(w_string(&key_string));
}

const json_ref& json_ref::get(const char* key) const {
  if (!json_is_object(ref_)) {
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
  if (!json_is_object(ref_)) {
    return defval;
  }
  auto object = json_to_object(ref_);
  auto it = object->findCString(key);
  if (it == object->map.end()) {
    return defval;
  }
  return it->second;
}

json_t *json_object_get(const json_t *json, const char *key)
{
    json_object_t *object;

    if(!json_is_object(json))
        return NULL;

    object = json_to_object(json);
    auto it = object->findCString(key);
    if (it == object->map.end()) {
      return nullptr;
    }
    return it->second;
}

int json_object_set_new_nocheck(
    json_t* json,
    const char* key,
    json_ref&& value) {
  json_object_t* object;

  if (!value)
    return -1;

  if (!key || !json_is_object(json) || json == value) {
    return -1;
  }
  object = json_to_object(json);

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

int json_object_set_new(json_t* json, const char* key, json_ref&& value) {
  if (!key || !utf8_check_string(key, -1)) {
    return -1;
  }

  return json_object_set_new_nocheck(json, key, std::move(value));
}

int json_object_del(json_t *json, const char *key)
{
    json_object_t *object;

    if(!json_is_object(json))
        return -1;

    object = json_to_object(json);
    auto it = object->findCString(key);
    if (it == object->map.end()) {
      return -1;
    }
    object->map.erase(it);
    return 0;
}

int json_object_clear(json_t *json)
{
    json_object_t *object;

    if(!json_is_object(json))
        return -1;

    object = json_to_object(json);
    object->map.clear();

    return 0;
}

int json_object_update(const json_t* src, json_t* target) {
  if (!json_is_object(src) || !json_is_object(src))
    return -1;

  auto target_obj = json_to_object(target);
  for (auto& it : json_to_object(src)->map) {
    target_obj->map[it.first] = it.second;
  }

  return 0;
}

int json_object_update_existing(const json_t* src, json_t* target) {
  if (!json_is_object(src) || !json_is_object(target))
    return -1;

  auto target_obj = json_to_object(target);
  for (auto& it : json_to_object(src)->map) {
    auto find = target_obj->map.find(it.first);
    if (find != target_obj->map.end()) {
      target_obj->map[it.first] = it.second;
    }
  }

  return 0;
}

int json_object_update_missing(const json_t* src, json_t* target) {
  if (!json_is_object(src) || !json_is_object(target))
    return -1;

  auto target_obj = json_to_object(target);
  for (auto& it : json_to_object(src)->map) {
    auto find = target_obj->map.find(it.first);
    if (find == target_obj->map.end()) {
      target_obj->map[it.first] = it.second;
    }
  }

  return 0;
}

static int json_object_equal(json_t* object1, json_t* object2) {
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

static json_ref json_object_copy(const json_t* object) {
  auto result = json_object();
  if (!result)
    return nullptr;

  json_object_update(object, result);

  return result;
}

static json_ref json_object_deep_copy(const json_t* object) {
  json_t* result;

  result = json_object();
  if (!result)
    return nullptr;

  auto target_obj = json_to_object(result);
  for (auto& it : json_to_object(object)->map) {
    target_obj->map[it.first] = json_deep_copy(it.second);
  }

  return result;
}


/*** array ***/

json_array_t::json_array_t(size_t sizeHint) : json(JSON_ARRAY) {
  table.reserve(std::max(sizeHint, size_t(8)));
}

json_array_t::json_array_t(std::initializer_list<json_ref> values)
    : json(JSON_ARRAY), table(values) {}

const std::vector<json_ref>& json_ref::array() const {
  if (!json_is_array(ref_)) {
    throw std::domain_error("json_ref::array() called for non-array");
  }
  return json_to_array(ref_)->table;
}

std::vector<json_ref>& json_ref::array() {
  if (!json_is_array(ref_)) {
    throw std::domain_error("json_ref::array() called for non-array");
  }
  return json_to_array(ref_)->table;
}

json_ref json_array_of_size(size_t nelems) {
  auto array = new json_array_t(nelems);
  return json_ref(&array->json, false);
}

json_ref json_array(void) {
  return json_array_of_size(8);
}

json_ref json_array(std::initializer_list<json_ref> values) {
  auto array = new json_array_t(std::move(values));
  return json_ref(&array->json, false);
}

int json_array_set_template(json_t *json, json_t *templ)
{
  return json_array_set_template_new(json, json_ref(templ));
}

int json_array_set_template_new(json_t* json, json_ref&& templ) {
  json_array_t* array;
  if (!json_is_array(json))
    return 0;
  array = json_to_array(json);
  array->templ = std::move(templ);
  return 1;
}

json_t *json_array_get_template(const json_t *array)
{
    if(!json_is_array(array))
        return 0;
    return json_to_array(array)->templ;
}

size_t json_array_size(const json_t *json)
{
    if(!json_is_array(json))
        return 0;

    return json_to_array(json)->table.size();
}

json_ref json_array_get(const json_t* json, size_t index) {
  if (!json_is_array(json)) {
    return nullptr;
  }
  auto array = json_to_array(json);

  if (index >= array->table.size()) {
    return nullptr;
  }

  return array->table[index];
}

int json_array_set_new(json_t* json, size_t index, json_ref&& value) {
  json_array_t* array;

  if (!value)
    return -1;

  if (!json_is_array(json) || json == value) {
    return -1;
  }
  array = json_to_array(json);

  if (index >= array->table.size()) {
    return -1;
  }

  array->table[index] = std::move(value);

  return 0;
}

int json_array_append_new(json_t* json, json_ref&& value) {
  json_array_t* array;

  if (!value)
    return -1;

  if (!json_is_array(json) || json == value) {
    return -1;
  }
  array = json_to_array(json);
  array->table.emplace_back(std::move(value));
  return 0;
}

int json_array_insert_new(json_t* json, size_t index, json_ref&& value) {
  if (!value)
    return -1;

  if (!json_is_array(json) || json == value) {
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

int json_array_remove(json_t *json, size_t index)
{
    json_array_t *array;

    if(!json_is_array(json))
        return -1;
    array = json_to_array(json);

    if (index > array->table.size()) {
      return -1;
    }

    auto it = array->table.begin();
    std::advance(it, index);

    array->table.erase(it);
    return 0;
}

int json_array_clear(json_t *json)
{
    if(!json_is_array(json))
        return -1;
    json_to_array(json)->table.clear();
    return 0;
}

int json_array_extend(json_t *json, json_t *other_json)
{
    json_array_t *array, *other;

    if(!json_is_array(json) || !json_is_array(other_json))
        return -1;
    array = json_to_array(json);
    other = json_to_array(other_json);

    array->table.insert(
        array->table.end(), other->table.begin(), other->table.end());

    return 0;
}

static int json_array_equal(json_t *array1, json_t *array2)
{
    size_t i, size;

    size = json_array_size(array1);
    if(size != json_array_size(array2))
        return 0;

    for(i = 0; i < size; i++)
    {
        json_t *value1, *value2;

        value1 = json_array_get(array1, i);
        value2 = json_array_get(array2, i);

        if(!json_equal(value1, value2))
            return 0;
    }

    return 1;
}

static json_ref json_array_copy(const json_t* array) {
  auto result = json_array();
  if (!result)
    return nullptr;

  auto& target_vector = json_to_array(result)->table;
  const auto& src_vector = json_to_array(array)->table;

  target_vector.insert(
      target_vector.begin(), src_vector.begin(), src_vector.end());

  return result;
}

static json_ref json_array_deep_copy(const json_t* array) {
  size_t i;

  auto result = json_array();
  if (!result)
    return nullptr;

  for (i = 0; i < json_array_size(array); i++)
    json_array_append_new(result, json_deep_copy(json_array_get(array, i)));

  return result;
}

/*** string ***/

json_string_t::json_string_t(const w_string& str)
    : json(JSON_STRING), value(str) {}

json_ref w_string_to_json(const w_string& str) {
  if (!str)
    return nullptr;

  auto string = new json_string_t(str);
  return json_ref(&string->json, false);
}

const char* json_string_value(const json_t* json) {
  json_string_t* jstr;

  if (!json_is_string(json))
    return NULL;

  jstr = json_to_string(json);
  jstr->value.makeNullTerminated();
  return jstr->value.c_str();
}

const w_string& json_to_w_string(const json_t* json) {
  if (!json_is_string(json)) {
    throw std::runtime_error("expected json string object");
  }

  return json_to_string(json)->value;
}

static int json_string_equal(json_t *string1, json_t *string2)
{
  return json_to_string(string1)->value == json_to_string(string2)->value;
}

static json_ref json_string_copy(const json_t* string) {
  return w_string_to_json(json_to_w_string(string));
}

/*** integer ***/

json_integer_t::json_integer_t(json_int_t value)
    : json(JSON_INTEGER), value(value) {}

json_ref json_integer(json_int_t value) {
  auto integer = new json_integer_t(value);
  return json_ref(&integer->json, false);
}

json_int_t json_integer_value(const json_t *json)
{
    if(!json_is_integer(json))
        return 0;

    return json_to_integer(json)->value;
}

int json_integer_set(json_t *json, json_int_t value)
{
    if(!json_is_integer(json))
        return -1;

    json_to_integer(json)->value = value;

    return 0;
}

static int json_integer_equal(json_t *integer1, json_t *integer2)
{
    return json_integer_value(integer1) == json_integer_value(integer2);
}

static json_t* json_integer_copy(const json_t* integer) {
  return json_integer(json_integer_value(integer));
}


/*** real ***/

json_real_t::json_real_t(double value) : json(JSON_REAL), value(value) {}

json_ref json_real(double value) {
  if (std::isnan(value) || std::isinf(value)) {
    return nullptr;
  }
  auto real = new json_real_t(value);
  return json_ref(&real->json, false);
}

double json_real_value(const json_t *json)
{
    if(!json_is_real(json))
        return 0;

    return json_to_real(json)->value;
}

int json_real_set(json_t* json, double value) {
  if (!json_is_real(json) || std::isnan(value) || std::isinf(value)) {
    return -1;
  }

  json_to_real(json)->value = value;

  return 0;
}

static int json_real_equal(json_t *real1, json_t *real2)
{
    return json_real_value(real1) == json_real_value(real2);
}

static json_t* json_real_copy(const json_t* real) {
  return json_real(json_real_value(real));
}


/*** number ***/

double json_number_value(const json_t *json)
{
    if(json_is_integer(json))
        return (double)json_integer_value(json);
    else if(json_is_real(json))
        return json_real_value(json);
    else
        return 0.0;
}


/*** simple values ***/

json_ref json_true(void) {
  static json_t the_true{JSON_TRUE, json_t::SingletonHack()};
  return &the_true;
}

json_ref json_false(void) {
  static json_t the_false{JSON_FALSE, json_t::SingletonHack()};
  return &the_false;
}

json_ref json_null(void) {
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

int json_equal(json_t *json1, json_t *json2)
{
    if(!json1 || !json2)
        return 0;

    if(json_typeof(json1) != json_typeof(json2))
        return 0;

    /* this covers true, false and null as they are singletons */
    if(json1 == json2)
        return 1;

    if(json_is_object(json1))
        return json_object_equal(json1, json2);

    if(json_is_array(json1))
        return json_array_equal(json1, json2);

    if(json_is_string(json1))
        return json_string_equal(json1, json2);

    if(json_is_integer(json1))
        return json_integer_equal(json1, json2);

    if(json_is_real(json1))
        return json_real_equal(json1, json2);

    return 0;
}


/*** copying ***/

json_ref json_copy(const json_t* json) {
  if (!json)
    return nullptr;

  if (json_is_object(json))
    return json_object_copy(json);

  if (json_is_array(json))
    return json_array_copy(json);

  if (json_is_string(json))
    return json_string_copy(json);

  if (json_is_integer(json))
    return json_integer_copy(json);

  if (json_is_real(json))
    return json_real_copy(json);

  if (json_is_true(json) || json_is_false(json) || json_is_null(json)) {
    return const_cast<json_t*>(json);
  }

  return nullptr;
}

json_ref json_deep_copy(const json_t* json) {
  if (!json)
    return nullptr;

  if (json_is_object(json))
    return json_object_deep_copy(json);

  if (json_is_array(json))
    return json_array_deep_copy(json);

  /* for the rest of the types, deep copying doesn't differ from
     shallow copying */

  if (json_is_string(json))
    return json_string_copy(json);

  if (json_is_integer(json))
    return json_integer_copy(json);

  if (json_is_real(json))
    return json_real_copy(json);

  if (json_is_true(json) || json_is_false(json) || json_is_null(json)) {
    return const_cast<json_t*>(json);
  }

  return nullptr;
}
