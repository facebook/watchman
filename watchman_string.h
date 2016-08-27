/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_STRING_H
#define WATCHMAN_STRING_H

#include <stdint.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

struct watchman_string;
typedef struct watchman_string w_string_t;

typedef enum {
  W_STRING_BYTE,
  W_STRING_UNICODE,
  W_STRING_MIXED
} w_string_type_t;

struct watchman_string {
  long refcnt;
  uint32_t _hval;
  uint32_t len;
  w_string_t *slice;
  const char *buf;
  w_string_type_t type:3;
  unsigned hval_computed:1;
};

uint32_t w_string_compute_hval(w_string_t *str);

static inline uint32_t w_string_hval(w_string_t *str) {
  if (str->hval_computed) {
    return str->_hval;
  }
  return w_string_compute_hval(str);
}

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

void w_string_in_place_normalize_separators(w_string_t **str, char target_sep);

w_string_t *w_string_make_printf(const char *format, ...);

/* Typed string creation functions. */
w_string_t *w_string_new_typed(const char *str,
    w_string_type_t type);
w_string_t *w_string_new_len_typed(const char *str, uint32_t len,
    w_string_type_t type);
w_string_t *w_string_new_len_no_ref_typed(const char *str, uint32_t len,
    w_string_type_t type);
w_string_t *w_string_new_basename_typed(const char *path,
    w_string_type_t type);
w_string_t *w_string_new_lower_typed(const char *str,
    w_string_type_t type);

void w_string_new_len_typed_stack(w_string_t *into, const char *str,
                                  uint32_t len, w_string_type_t type);

#ifdef _WIN32
w_string_t *w_string_new_wchar_typed(WCHAR *str, int len,
    w_string_type_t type);
#endif
w_string_t *w_string_normalize_separators(w_string_t *str, char target_sep);

w_string_t *w_string_path_cat(w_string_t *parent, w_string_t *rhs);
w_string_t *w_string_path_cat_cstr(w_string_t *parent, const char *rhs);
w_string_t *w_string_path_cat_cstr_len(w_string_t *parent, const char *rhs,
                                       uint32_t rhs_len);
bool w_string_path_is_absolute(const w_string_t *str);

bool w_string_startswith(w_string_t *str, w_string_t *prefix);
bool w_string_startswith_caseless(w_string_t *str, w_string_t *prefix);
w_string_t *w_string_shell_escape(const w_string_t *str);
w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len);
w_string_t *w_string_suffix(w_string_t *str);
bool w_string_suffix_match(w_string_t *str, w_string_t *suffix);

bool w_string_is_known_unicode(w_string_t *str);
bool w_string_is_null_terminated(w_string_t *str);
size_t w_string_strlen(w_string_t *str);

uint32_t strlen_uint32(const char *str);
uint32_t w_hash_bytes(const void *key, size_t length, uint32_t initval);

uint32_t w_string_embedded_size(w_string_t *str);
void w_string_embedded_copy(w_string_t *dest, w_string_t *src);

struct watchman_dir;
w_string_t *w_dir_copy_full_path(struct watchman_dir *dir);
w_string_t *w_dir_path_cat_cstr_len(struct watchman_dir *dir, const char *extra,
                                    uint32_t extra_len);
w_string_t *w_dir_path_cat_cstr(struct watchman_dir *dir, const char *extra);
w_string_t *w_dir_path_cat_str(struct watchman_dir *dir, w_string_t *str);

bool w_is_path_absolute_cstr(const char *path);
bool w_is_path_absolute_cstr_len(const char *path, uint32_t len);

static inline bool is_slash(char c) {
  return (c == '/') || (c == '\\');
}

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */
