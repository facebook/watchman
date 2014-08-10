/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <stdarg.h>

w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len)
{
  w_string_t *slice;

  if (start == 0 && len == str->len) {
    w_string_addref(str);
    return str;
  }

  if (start > str->len || start + len > str->len) {
    errno = EINVAL;
    w_log(W_LOG_FATAL,
        "illegal string slice start=%" PRIu32 " len=%" PRIu32
        " but str->len=%" PRIu32 "\nstring={%.*s}\n",
        start, len, str->len, str->len, str->buf);
    return NULL;
  }

  slice = calloc(1, sizeof(*str));
  slice->refcnt = 1;
  slice->len = len;
  slice->buf = str->buf + start;
  slice->slice = str;
  slice->hval = w_hash_bytes(slice->buf, slice->len, 0);

  w_string_addref(str);
  return slice;
}


w_string_t *w_string_new(const char *str)
{
  w_string_t *s;
  uint32_t len = strlen(str);
  uint32_t hval = w_hash_bytes(str, len, 0);
  char *buf;

  s = malloc(sizeof(*s) + len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->hval = hval;
  s->len = len;
  s->slice = NULL;
  buf = (char*)(s + 1);
  memcpy(buf, str, len);
  buf[len] = 0;
  s->buf = buf;

  return s;
}

w_string_t *w_string_make_printf(const char *format, ...)
{
  w_string_t *s;
  int len;
  char *buf;
  va_list args;

  va_start(args, format);
  // Get the length needed
  len = vsnprintf(NULL, 0, format, args);
  va_end(args);

  s = malloc(sizeof(*s) + len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->len = len;
  s->slice = NULL;
  buf = (char*)(s + 1);
  va_start(args, format);
  vsnprintf(buf, len + 1, format, args);
  va_end(args);
  s->buf = buf;
  s->hval = w_hash_bytes(buf, len, 0);

  return s;
}

/* return a reference to a lowercased version of a string */
w_string_t *w_string_dup_lower(w_string_t *str)
{
  bool is_lower = true;
  char *buf;
  uint32_t i;
  w_string_t *s;

  for (i = 0; i < str->len; i++) {
    if (tolower((unsigned char)str->buf[i]) != str->buf[i]) {
      is_lower = false;
      break;
    }
  }

  if (is_lower) {
    w_string_addref(str);
    return str;
  }

  /* need to make a lowercase version */

  s = malloc(sizeof(*s) + str->len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->len = str->len;
  s->slice = NULL;
  buf = (char*)(s + 1);
  for (i = 0; i < str->len; i++) {
    buf[i] = tolower((unsigned char)str->buf[i]);
  }
  buf[str->len] = 0;
  s->buf = buf;
  s->hval = w_hash_bytes(buf, str->len, 0);

  return s;
}

/* make a lowercased copy of string */
w_string_t *w_string_new_lower(const char *str)
{
  w_string_t *s;
  uint32_t len = strlen(str);
  char *buf;
  uint32_t i;

  s = malloc(sizeof(*s) + len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->len = len;
  s->slice = NULL;
  buf = (char*)(s + 1);
  // TODO: optionally use ICU
  for (i = 0; i < len; i++) {
    buf[i] = tolower((unsigned char)str[i]);
  }
  buf[len] = 0;
  s->buf = buf;
  s->hval = w_hash_bytes(buf, len, 0);

  return s;
}

void w_string_addref(w_string_t *str)
{
  w_refcnt_add(&str->refcnt);
}

void w_string_delref(w_string_t *str)
{
  if (!w_refcnt_del(&str->refcnt)) return;
  if (str->slice) w_string_delref(str->slice);
  free(str);
}

int w_string_compare(const w_string_t *a, const w_string_t *b)
{
  if (a == b) return 0;
  return strcmp(a->buf, b->buf);
}

bool w_string_equal_cstring(const w_string_t *a, const char *b)
{
  uint32_t blen = strlen(b);
  if (a->len != blen) return false;
  return memcmp(a->buf, b, a->len) == 0 ? true : false;
}

bool w_string_equal(const w_string_t *a, const w_string_t *b)
{
  if (a == b) return true;
  if (a->hval != b->hval) return false;
  if (a->len != b->len) return false;
  return memcmp(a->buf, b->buf, a->len) == 0 ? true : false;
}

bool w_string_equal_caseless(const w_string_t *a, const w_string_t *b)
{
  uint32_t i;
  if (a == b) return true;
  if (a->len != b->len) return false;
  for (i = 0; i < a->len; i++) {
    if (tolower((unsigned char)a->buf[i]) != tolower((unsigned char)b->buf[i])) {
      return false;
    }
  }
  return true;
}

w_string_t *w_string_dirname(w_string_t *str)
{
  int end;

  /* can't use libc strXXX functions because we may be operating
   * on a slice */
  for (end = str->len - 1; end >= 0; end--) {
    if (str->buf[end] == '/') {
      /* found the end of the parent dir */
      return w_string_slice(str, 0, end);
    }
  }

  return NULL;
}

bool w_string_suffix_match(w_string_t *str, w_string_t *suffix)
{
  unsigned int base, i;

  if (str->len < suffix->len + 1) {
    return false;
  }

  base = str->len - suffix->len;

  if (str->buf[base - 1] != '.') {
    return false;
  }

  for (i = 0; i < suffix->len; i++) {
    if (tolower((unsigned char)str->buf[base + i]) != suffix->buf[i]) {
      return false;
    }
  }

  return true;
}

// Return the normalized (lowercase) filename suffix
w_string_t *w_string_suffix(w_string_t *str)
{
  int end;
  char name_buf[128];
  char *buf;

  /* can't use libc strXXX functions because we may be operating
   * on a slice */
  for (end = str->len - 1; end >= 0; end--) {
    if (str->buf[end] == '.') {
      if (str->len - (end + 1) >= sizeof(name_buf) - 1) {
        // Too long
        return NULL;
      }

      buf = name_buf;
      end++;
      while ((unsigned)end < str->len) {
        *buf = tolower((unsigned char)str->buf[end]);
        end++;
        buf++;
      }
      *buf = '\0';
      return w_string_new(name_buf);
    }

    if (str->buf[end] == '/') {
      // No suffix
      return NULL;
    }
  }

  // Has no suffix
  return NULL;
}

bool w_string_startswith(w_string_t *str, w_string_t *prefix)
{
  if (prefix->len > str->len) {
    return false;
  }
  return memcmp(str->buf, prefix->buf, prefix->len) == 0;
}

w_string_t *w_string_canon_path(w_string_t *str)
{
  int end;
  int trim = 0;

  for (end = str->len - 1; end >= 0 && str->buf[end] == '/'; end--) {
    trim++;
  }
  if (trim) {
    return w_string_slice(str, 0, str->len - trim);
  }
  w_string_addref(str);
  return str;
}


w_string_t *w_string_basename(w_string_t *str)
{
  int end;

  /* can't use libc strXXX functions because we may be operating
   * on a slice */
  for (end = str->len - 1; end >= 0; end--) {
    if (str->buf[end] == '/') {
      /* found the end of the parent dir */
      return w_string_slice(str, end + 1, str->len - (end + 1));
    }
  }

  w_string_addref(str);
  return str;
}

w_string_t *w_string_path_cat(w_string_t *parent, w_string_t *rhs)
{
  char name_buf[WATCHMAN_NAME_MAX];

  if (rhs->len == 0) {
    w_string_addref(parent);
    return parent;
  }

  snprintf(name_buf, sizeof(name_buf), "%.*s/%.*s",
      parent->len, parent->buf,
      rhs->len, rhs->buf);

  return w_string_new(name_buf);
}

char *w_string_dup_buf(const w_string_t *str)
{
  char *buf;

  buf = malloc(str->len + 1);
  if (!buf) {
    return NULL;
  }

  memcpy(buf, str->buf, str->len);
  buf[str->len] = 0;

  return buf;
}

// Given a json array, concat the elements using a delimiter
w_string_t *w_string_implode(json_t *arr, const char *delim)
{
  uint32_t delim_len = strlen(delim);
  uint32_t len = 0;
  uint32_t i;
  w_string_t *s;
  char *buf;

  if (json_array_size(arr) == 0) {
    return w_string_new("");
  }

  if (json_array_size(arr) == 1) {
    return w_string_new(json_string_value(json_array_get(arr, 0)));
  }

  len = (json_array_size(arr) - 1) * delim_len;

  for (i = 0; i < json_array_size(arr); i++) {
    const char *str;

    str = json_string_value(json_array_get(arr, i));
    len += strlen(str);
  }

  s = malloc(sizeof(*s) + len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->slice = NULL;
  buf = (char*)(s + 1);
  s->buf = buf;

  for (i = 0; i < json_array_size(arr); i++) {
    const char *str;
    uint32_t l;

    str = json_string_value(json_array_get(arr, i));
    l = strlen(str);

    memcpy(buf, str, l);

    // Final string doesn't want delimiter after it
    if (i == json_array_size(arr) - 1) {
      buf += l;
      break;
    }

    memcpy(buf + l, delim, delim_len);
    buf += l + delim_len;
  }
  *buf = '\0';

  s->len = buf - s->buf;
  s->hval = w_hash_bytes(s->buf, s->len, 0);

  return s;
}

// Given a string, return a shell-escaped copy
w_string_t *w_string_shell_escape(const w_string_t *str)
{
  // Worst case expansion for a char is 4x, plus quoting either end
  uint32_t len = 2 + (str->len * 4);
  w_string_t *s;
  char *buf;
  const char *src, *end;

  s = malloc(sizeof(*s) + len + 1);
  if (!s) {
    perror("no memory available");
    abort();
  }

  s->refcnt = 1;
  s->slice = NULL;
  buf = (char*)(s + 1);
  s->buf = buf;

  src = str->buf;
  end = src + str->len;

  *buf = '\'';
  buf++;
  while (src < end) {
    if (*src == '\'') {
      memcpy(buf, "'\\''", 4);
      buf += 4;
    } else {
      *buf = *src;
      buf++;
    }
    src++;
  }
  *buf = '\'';
  buf++;
  *buf = 0;
  s->len = buf - s->buf;
  s->hval = w_hash_bytes(s->buf, s->len, 0);

  return s;
}

/* vim:ts=2:sw=2:et:
 */
