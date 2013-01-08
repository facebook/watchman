/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len)
{
  w_string_t *slice;

  if (start == 0 && len == str->len) {
    w_string_addref(str);
    return str;
  }

  if (start >= str->len || start + len > str->len) {
    errno = EINVAL;
    abort();
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

bool w_string_equal(const w_string_t *a, const w_string_t *b)
{
  if (a == b) return true;
  if (a->hval != b->hval) return false;
  if (a->len != b->len) return false;
  return memcmp(a->buf, b->buf, a->len) == 0 ? true : false;
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

  abort();

  return NULL;
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

  abort();

  return NULL;
}

w_string_t *w_string_path_cat(const w_string_t *parent, const w_string_t *rhs)
{
  char name_buf[WATCHMAN_NAME_MAX];

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

/* vim:ts=2:sw=2:et:
 */

