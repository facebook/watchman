/* @nolint
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef JANSSON_PRIVATE_H
#define JANSSON_PRIVATE_H

#include <stddef.h>
#include "jansson.h"
#include "strbuffer.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

#define container_of(ptr_, type_, member_)  \
    ((type_ *)((char *)ptr_ - offsetof(type_, member_)))

/* va_copy is a C99 feature. In C89 implementations, it's sometimes
   available as __va_copy. If not, memcpy() should do the trick. */
#ifndef va_copy
#ifdef __va_copy
#define va_copy __va_copy
#else
#define va_copy(a, b)  memcpy(&(a), &(b), sizeof(va_list))
#endif
#endif

struct json_object_t {
    json_t json;
    std::unordered_map<w_string, json_ref> map;

    json_object_t(size_t sizeHint = 0);

    typename std::unordered_map<w_string, json_ref>::iterator findCString(
        const char* key);
};

struct json_array_t {
    json_t json;
    std::vector<json_ref> table;
    json_ref templ;

    json_array_t(size_t sizeHint = 0);
    json_array_t(std::initializer_list<json_ref> values);
};

struct json_string_t {
    json_t json;
    w_string value;

    json_string_t(const w_string& str);
};

struct json_real_t {
    json_t json;
    double value;

    json_real_t(double value);
};

struct json_integer_t {
  json_t json;
  json_int_t value;

  json_integer_t(json_int_t value);
};

#define json_to_object(json_)  container_of((json_t*)json_, json_object_t, json)
#define json_to_array(json_)   container_of((json_t*)json_, json_array_t, json)
#define json_to_string(json_)  container_of((json_t*)json_, json_string_t, json)
#define json_to_real(json_)   container_of((json_t*)json_, json_real_t, json)
#define json_to_integer(json_) container_of((json_t*)json_, json_integer_t, json)

void jsonp_error_init(json_error_t *error, const char *source);
void jsonp_error_set_source(json_error_t *error, const char *source);
void jsonp_error_set(json_error_t *error, int line, int column,
                     size_t position, const char *msg, ...);
void jsonp_error_vset(json_error_t *error, int line, int column,
                      size_t position, const char *msg, va_list ap);

/* Locale independent string<->double conversions */
int jsonp_strtod(strbuffer_t *strbuffer, double *out);
int jsonp_dtostr(char *buffer, size_t size, double value);

/* Wrappers for custom memory functions */
void* jsonp_malloc(size_t size);
void jsonp_free(void *ptr);
char *jsonp_strdup(const char *str);

/* Windows compatibility */
#ifdef _WIN32
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#endif

#endif
