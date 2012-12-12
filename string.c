/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"

// In my profiler runs turning off interning and enabling slicing
// gives a 2-3% win for www.
// Not huge, but better than not having it :-/

// Not sure if the interning is worth it; you can bench it for yourself
// by setting this to 1 and running some tests.
#define DO_STRING_INTERN 0
// Not sure if slicing into strings for basename/dirname is worth it...
// so play with this to find out.
#define USE_SLICES 1

#if DO_STRING_INTERN
static struct watchman_hash_funcs intern_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  NULL
};

static w_ht_t *intern = NULL;
static pthread_rwlock_t intern_lock = PTHREAD_RWLOCK_INITIALIZER;
#endif

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


#define CAN_INTERN(len)   (len <= WATCHMAN_NAME_MAX)

w_string_t *w_string_new(const char *str)
{
  w_string_t *s;
  uint32_t len = strlen(str);
  uint32_t hval = w_hash_bytes(str, len, 0);
  char *buf;

#if DO_STRING_INTERN
  if (CAN_INTERN(len) && intern) {
    w_string_t fake;
    bool found;

    fake.hval = hval;
    fake.len = len;
    fake.buf = str;

    pthread_rwlock_rdlock(&intern_lock);
    found = w_ht_lookup(intern, (w_ht_val_t)&fake, (w_ht_val_t*)&s, true);
    pthread_rwlock_unlock(&intern_lock);
    if (found) {
      /* we only want the keys to be ref'd, otherwise we can't tell when
       * we are safe to collect dead ones.
       * Therefore, we need to add a ref for the caller here now */
      w_string_addref(s);
      return s;
    }
  }
#endif

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

#if DO_STRING_INTERN
  if (CAN_INTERN(len)) {
    pthread_rwlock_wrlock(&intern_lock);
    if (!intern) {
      intern = w_ht_new(256*1024, &w_ht_string_funcs);
    }
    w_ht_set(intern, (w_ht_val_t)s, (w_ht_val_t)s);
    pthread_rwlock_unlock(&intern_lock);
  //  w_log(W_LOG_DBG, "INTERN: %d entries\n", w_ht_size(intern));
  } else {
    w_log(W_LOG_DBG, "INTERN: skipping string of len %d %s\n",
        len, s->buf);
  }
#endif

  return s;
}

/* run garbage collection over interned string set */
void w_string_collect(void)
{
#if DO_STRING_INTERN
  w_ht_iter_t iter;
  int min_ref = 0;
  int max_ref = 0;
  int count = 0;
  int deleted = 0;
  uint32_t mem = 0;
  w_string_t *mstr = NULL;

  pthread_rwlock_wrlock(&intern_lock);
  if (w_ht_first(intern, &iter)) do {
    w_string_t *str = (w_string_t*)iter.value;

    assert(str->refcnt >= 1);

    if (str->refcnt == 1) {
      /* we're the only one referencing it; garbage! */
      w_ht_iter_del(intern, &iter);
      deleted++;
    } else {
      mem += str->len;

      if (count == 0 || str->refcnt < min_ref) {
        min_ref = str->refcnt;
      }
      if (count == 0 || str->refcnt > max_ref) {
        max_ref = str->refcnt;
        mstr = str;
      }
      count++;
    }

  } while (w_ht_next(intern, &iter));
  pthread_rwlock_unlock(&intern_lock);

  w_log(W_LOG_DBG,
      "string collect: deleted=%d live=%d mem=%u min_ref=%d max_ref=%d %s\n",
      deleted, count, mem, min_ref, max_ref, mstr->buf);
#endif
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
#if USE_SLICES
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
#else
  char buf[WATCHMAN_NAME_MAX];

  memcpy(buf, str->buf, str->len);
  buf[str->len] = 0;

  return w_string_new(dirname(buf));
#endif
}

w_string_t *w_string_basename(w_string_t *str)
{
#if USE_SLICES
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
#else
  char buf[WATCHMAN_NAME_MAX];

  memcpy(buf, str->buf, str->len);
  buf[str->len] = 0;

  return w_string_new(basename(buf));
#endif
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

