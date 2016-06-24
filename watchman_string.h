/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_STRING_H
#define WATCHMAN_STRING_H

#include "jansson.h" // we use json_t in here

#ifdef __cplusplus
extern "C" {
#endif

struct watchman_string;
typedef struct watchman_string w_string_t;

struct watchman_string {
  long refcnt;
  uint32_t hval;
  uint32_t len;
  w_string_t *slice;
  const char *buf;
};

void w_string_addref(w_string_t *str);

w_string_t *w_string_basename(w_string_t *str);

w_string_t *w_string_canon_path(w_string_t *str);
int w_string_compare(const w_string_t *a, const w_string_t *b);
bool w_string_contains_cstr_len(w_string_t *str, const char *needle,
                                uint32_t nlen);

void w_string_delref(w_string_t *str);
w_string_t *w_string_dirname(w_string_t *str);
char *w_string_dup_buf(const w_string_t *str);
w_string_t *w_string_dup_lower(w_string_t *str);

bool w_string_equal(const w_string_t *a, const w_string_t *b);
bool w_string_equal_caseless(const w_string_t *a, const w_string_t *b);
bool w_string_equal_cstring(const w_string_t *a, const char *b);

w_string_t *w_string_implode(json_t *arr, const char *delim);
void w_string_in_place_normalize_separators(w_string_t **str, char target_sep);

w_string_t *w_string_make_printf(const char *format, ...);

w_string_t *w_string_new(const char *str);
w_string_t *w_string_new_basename(const char *path);
w_string_t *w_string_new_len(const char *str, uint32_t len);
w_string_t *w_string_new_lower(const char *str);
#ifdef _WIN32
w_string_t *w_string_new_wchar(WCHAR *str, int len);
#endif
w_string_t *w_string_normalize_separators(w_string_t *str, char target_sep);

w_string_t *w_string_path_cat(w_string_t *parent, w_string_t *rhs);
w_string_t *w_string_path_cat_cstr(w_string_t *parent, const char *rhs);
w_string_t *w_string_path_cat_cstr_len(w_string_t *parent, const char *rhs,
                                       uint32_t rhs_len);

bool w_string_startswith(w_string_t *str, w_string_t *prefix);
bool w_string_startswith_caseless(w_string_t *str, w_string_t *prefix);
w_string_t *w_string_shell_escape(const w_string_t *str);
w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len);
w_string_t *w_string_suffix(w_string_t *str);
bool w_string_suffix_match(w_string_t *str, w_string_t *suffix);

json_t *w_string_to_json(w_string_t *str);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
