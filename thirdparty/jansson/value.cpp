/* @nolint
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>

#include "jansson.h"
#include "jansson_private.h"
#include "utf.h"
#include "watchman_string.h"

json_t::json_t(json_type type) : type(type), refcount(1) {}

json_t::json_t(json_type type, json_t::SingletonHack&&)
    : type(type), refcount(-1) {}

/*** object ***/

json_object_t::json_object_t(size_t sizeHint) : json(JSON_OBJECT), visited(0) {
  map.reserve(sizeHint);
}

json_t *json_object_of_size(size_t size)
{
    auto object = new json_object_t(size);
    return &object->json;
}

json_t *json_object(void)
{
    return json_object_of_size(0);
}

json_object_t::~json_object_t() {
  for (auto& it : map) {
    json_decref(it.second);
  }
}

size_t json_object_size(const json_t *json)
{
    json_object_t *object;

    if(!json_is_object(json))
        return 0;

    object = json_to_object(json);
    return object->map.size();
}

typename std::unordered_map<w_string, json_t*>::iterator
json_object_t::findCString(const char* key) {
  // Avoid making a copy of the string for this lookup
  w_string_t key_string;
  w_string_new_len_typed_stack(
      &key_string, key, strlen_uint32(key), W_STRING_BYTE);
  return map.find(w_string(&key_string));
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

int json_object_set_new_nocheck(json_t *json, const char *key, json_t *value)
{
    json_object_t *object;

    if(!value)
        return -1;

    if(!key || !json_is_object(json) || json == value)
    {
        json_decref(value);
        return -1;
    }
    object = json_to_object(json);

    w_string key_string(key);
    object->map[key_string] = value;
    return 0;
}

int json_object_set_new(json_t *json, const char *key, json_t *value)
{
    if(!key || !utf8_check_string(key, -1))
    {
        json_decref(value);
        return -1;
    }

    return json_object_set_new_nocheck(json, key, value);
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

    for (auto& it : object->map) {
      json_decref(it.second);
    }
    object->map.clear();

    return 0;
}

int json_object_update(json_t *object, json_t *other)
{
    if(!json_is_object(object) || !json_is_object(other))
        return -1;

    auto target_obj = json_to_object(other);
    for (auto& it : json_to_object(object)->map) {
      target_obj->map[it.first] = it.second;
      json_incref(it.second);
    }

    return 0;
}

int json_object_update_existing(json_t *object, json_t *other)
{
    if(!json_is_object(object) || !json_is_object(other))
        return -1;

    auto target_obj = json_to_object(other);
    for (auto& it : json_to_object(object)->map) {
      auto find = target_obj->map.find(it.first);
      if (find != target_obj->map.end()) {
        target_obj->map[it.first] = it.second;
        json_incref(it.second);
      }
    }

    return 0;
}

int json_object_update_missing(json_t *object, json_t *other)
{
    if(!json_is_object(object) || !json_is_object(other))
        return -1;

    auto target_obj = json_to_object(other);
    for (auto& it : json_to_object(object)->map) {
      auto find = target_obj->map.find(it.first);
      if (find == target_obj->map.end()) {
        target_obj->map[it.first] = it.second;
        json_incref(it.second);
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

static json_t *json_object_copy(json_t *object)
{
    auto result = json_object();
    if(!result)
        return NULL;

    json_object_update(result, object);

    return result;
}

static json_t *json_object_deep_copy(json_t *object)
{
    json_t *result;

    result = json_object();
    if(!result)
        return NULL;

    auto target_obj = json_to_object(result);
    for (auto& it : json_to_object(object)->map) {
      target_obj->map[it.first] = json_deep_copy(it.second);
    }

    return result;
}


/*** array ***/

json_array_t::json_array_t(size_t sizeHint)
    : json(JSON_ARRAY),
      size(std::max(sizeHint, size_t(8))),
      entries(0),
      table(nullptr),
      visited(0),
      templ(nullptr) {
  table = (json_t**)jsonp_malloc(size * sizeof(json_t*));
  if (!table) {
    throw std::bad_alloc();
  }
}

json_array_t::~json_array_t() {
  size_t i;

  for (i = 0; i < entries; i++)
    json_decref(table[i]);

  if (templ) {
    json_decref(templ);
  }
  jsonp_free(table);
}

json_t *json_array_of_size(size_t nelems)
{
    auto array = new json_array_t(nelems);
    return &array->json;
}

json_t *json_array(void)
{
    return json_array_of_size(8);
}

int json_array_set_template(json_t *json, json_t *templ)
{
  if (json_array_set_template_new(json, templ)) {
    if (templ) {
      json_incref(templ);
    }
    return 1;
  }
  return 0;
}

int json_array_set_template_new(json_t *json, json_t *templ)
{
    json_array_t *array;
    if(!json_is_array(json))
        return 0;
    array = json_to_array(json);
    if (array->templ) {
      json_decref(array->templ);
    }
    array->templ = templ;
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

    return json_to_array(json)->entries;
}

json_t *json_array_get(const json_t *json, size_t index)
{
    json_array_t *array;
    if(!json_is_array(json))
        return NULL;
    array = json_to_array(json);

    if(index >= array->entries)
        return NULL;

    return array->table[index];
}

int json_array_set_new(json_t *json, size_t index, json_t *value)
{
    json_array_t *array;

    if(!value)
        return -1;

    if(!json_is_array(json) || json == value)
    {
        json_decref(value);
        return -1;
    }
    array = json_to_array(json);

    if(index >= array->entries)
    {
        json_decref(value);
        return -1;
    }

    json_decref(array->table[index]);
    array->table[index] = value;

    return 0;
}

static void array_move(json_array_t *array, size_t dest,
                       size_t src, size_t count)
{
    memmove(&array->table[dest], &array->table[src], count * sizeof(json_t *));
}

static void array_copy(json_t **dest, size_t dpos,
                       json_t **src, size_t spos,
                       size_t count)
{
    memcpy(&dest[dpos], &src[spos], count * sizeof(json_t *));
}

static json_t **json_array_grow(json_array_t *array,
                                size_t amount,
                                int copy)
{
    size_t new_size;
    json_t **old_table, **new_table;

    if(array->entries + amount <= array->size)
        return array->table;

    old_table = array->table;

    new_size = std::max(array->size + amount, array->size * 2);
    new_table = (json_t**)jsonp_malloc(new_size * sizeof(json_t *));
    if(!new_table)
        return NULL;

    array->size = new_size;
    array->table = new_table;

    if(copy) {
        array_copy(array->table, 0, old_table, 0, array->entries);
        jsonp_free(old_table);
        return array->table;
    }

    return old_table;
}

int json_array_append_new(json_t *json, json_t *value)
{
    json_array_t *array;

    if(!value)
        return -1;

    if(!json_is_array(json) || json == value)
    {
        json_decref(value);
        return -1;
    }
    array = json_to_array(json);

    if(!json_array_grow(array, 1, 1)) {
        json_decref(value);
        return -1;
    }

    array->table[array->entries] = value;
    array->entries++;

    return 0;
}

int json_array_insert_new(json_t *json, size_t index, json_t *value)
{
    json_array_t *array;
    json_t **old_table;

    if(!value)
        return -1;

    if(!json_is_array(json) || json == value) {
        json_decref(value);
        return -1;
    }
    array = json_to_array(json);

    if(index > array->entries) {
        json_decref(value);
        return -1;
    }

    old_table = json_array_grow(array, 1, 0);
    if(!old_table) {
        json_decref(value);
        return -1;
    }

    if(old_table != array->table) {
        array_copy(array->table, 0, old_table, 0, index);
        array_copy(array->table, index + 1, old_table, index,
                   array->entries - index);
        jsonp_free(old_table);
    }
    else
        array_move(array, index + 1, index, array->entries - index);

    array->table[index] = value;
    array->entries++;

    return 0;
}

int json_array_remove(json_t *json, size_t index)
{
    json_array_t *array;

    if(!json_is_array(json))
        return -1;
    array = json_to_array(json);

    if(index >= array->entries)
        return -1;

    json_decref(array->table[index]);

    array_move(array, index, index + 1, array->entries - index);
    array->entries--;

    return 0;
}

int json_array_clear(json_t *json)
{
    json_array_t *array;
    size_t i;

    if(!json_is_array(json))
        return -1;
    array = json_to_array(json);

    for(i = 0; i < array->entries; i++)
        json_decref(array->table[i]);

    array->entries = 0;
    return 0;
}

int json_array_extend(json_t *json, json_t *other_json)
{
    json_array_t *array, *other;
    size_t i;

    if(!json_is_array(json) || !json_is_array(other_json))
        return -1;
    array = json_to_array(json);
    other = json_to_array(other_json);

    if(!json_array_grow(array, other->entries, 1))
        return -1;

    for(i = 0; i < other->entries; i++)
        json_incref(other->table[i]);

    array_copy(array->table, array->entries, other->table, 0, other->entries);

    array->entries += other->entries;
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

static json_t *json_array_copy(json_t *array)
{
    json_t *result;
    size_t i;

    result = json_array();
    if(!result)
        return NULL;

    for(i = 0; i < json_array_size(array); i++)
        json_array_append(result, json_array_get(array, i));

    return result;
}

static json_t *json_array_deep_copy(json_t *array)
{
    json_t *result;
    size_t i;

    result = json_array();
    if(!result)
        return NULL;

    for(i = 0; i < json_array_size(array); i++)
        json_array_append_new(result, json_deep_copy(json_array_get(array, i)));

    return result;
}

/*** string ***/

json_string_t::json_string_t(w_string_t* str)
    : json(JSON_STRING), value(str), cache(nullptr) {
  w_string_addref(str);
}

json_string_t::~json_string_t() {
  w_string_delref(value);
  free(cache);
}

json_t *w_string_to_json(w_string_t *str)
{
    if(!str)
        return NULL;

    auto string = new json_string_t(str);
    return &string->json;
}

json_t *typed_string_len_to_json(const char *str, size_t len, w_string_type_t type)
{
    return w_string_to_json(w_string_new_len_no_ref_typed(str, len, type));
}

json_t *typed_string_to_json(const char *str, w_string_type_t type)
{
    return typed_string_len_to_json(str, strlen(str), type);
}

const char *json_string_value(const json_t *json)
{
    json_string_t *jstr;
    w_string_t *value;
    char *buf;

    if(!json_is_string(json))
        return NULL;

    jstr = json_to_string(json);
    value = jstr->value;

    if (w_string_is_null_terminated(value)) {
        // Safe to return the buffer itself
        return value->buf;
    }

    // If it's not null-terminated, use a cached version that is null-terminated

    if (jstr->cache) {
        return jstr->cache;
    }

    buf = w_string_dup_buf(value);
    if (!buf) {
        return NULL;
    }
    jstr->cache = buf;
    return buf;
}

w_string_t *json_to_w_string(const json_t *json)
{
    json_string_t *jstr;

    if (!json_is_string(json)) {
        return NULL;
    }

    jstr = json_to_string(json);

    if (!jstr) {
        return NULL;
    }

    return jstr->value;
}

w_string_t *json_to_w_string_incref(const json_t *json)
{
    w_string_t *str = json_to_w_string(json);

    if (!str) {
        return NULL;
    }

    w_string_addref(str);

    return str;
}

static int json_string_equal(json_t *string1, json_t *string2)
{
    return strcmp(json_string_value(string1), json_string_value(string2)) == 0;
}

static json_t *json_string_copy(json_t *string)
{
    return w_string_to_json(json_to_w_string(string));
}

/*** integer ***/

json_integer_t::json_integer_t(json_int_t value)
    : json(JSON_INTEGER), value(value) {}

json_t *json_integer(json_int_t value)
{
    auto integer = new json_integer_t(value);
    return &integer->json;
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

static json_t *json_integer_copy(json_t *integer)
{
    return json_integer(json_integer_value(integer));
}


/*** real ***/

json_real_t::json_real_t(double value) : json(JSON_REAL), value(value) {}

json_t* json_real(double value) {
  if (std::isnan(value) || std::isinf(value)) {
    return nullptr;
  }

  auto real = new json_real_t(value);
  return &real->json;
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

static json_t *json_real_copy(json_t *real)
{
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

json_t *json_true(void)
{
  static json_t the_true{JSON_TRUE, json_t::SingletonHack()};
  return &the_true;
}


json_t *json_false(void)
{
  static json_t the_false{JSON_FALSE, json_t::SingletonHack()};
  return &the_false;
}


json_t *json_null(void)
{
  static json_t the_null{JSON_NULL, json_t::SingletonHack()};
  return &the_null;
}


/*** deletion ***/

void json_delete(json_t *json)
{
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

json_t *json_copy(json_t *json)
{
    if(!json)
        return NULL;

    if(json_is_object(json))
        return json_object_copy(json);

    if(json_is_array(json))
        return json_array_copy(json);

    if(json_is_string(json))
        return json_string_copy(json);

    if(json_is_integer(json))
        return json_integer_copy(json);

    if(json_is_real(json))
        return json_real_copy(json);

    if(json_is_true(json) || json_is_false(json) || json_is_null(json))
        return json;

    return NULL;
}

json_t *json_deep_copy(json_t *json)
{
    if(!json)
        return NULL;

    if(json_is_object(json))
        return json_object_deep_copy(json);

    if(json_is_array(json))
        return json_array_deep_copy(json);

    /* for the rest of the types, deep copying doesn't differ from
       shallow copying */

    if(json_is_string(json))
        return json_string_copy(json);

    if(json_is_integer(json))
        return json_integer_copy(json);

    if(json_is_real(json))
        return json_real_copy(json);

    if(json_is_true(json) || json_is_false(json) || json_is_null(json))
        return json;

    return NULL;
}
