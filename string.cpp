/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <stdarg.h>
#include <new>
#include <stdexcept>

// string piece

w_string_piece::w_string_piece() : s_(nullptr), e_(nullptr) {}
w_string_piece::w_string_piece(std::nullptr_t) : s_(nullptr), e_(nullptr) {}

w_string_piece::w_string_piece(w_string_piece&& other) noexcept
    : s_(other.s_), e_(other.e_) {
  other.s_ = nullptr;
  other.e_ = nullptr;
}

w_string w_string_piece::asWString(w_string_type_t stringType) const {
  return w_string(data(), size(), stringType);
}

bool w_string_piece::pathIsAbsolute() const {
  return w_is_path_absolute_cstr_len(data(), size());
}

w_string_piece w_string_piece::dirName() const {
  for (auto end = e_; end >= s_; --end) {
    if (*end == WATCHMAN_DIR_SEP) {
      /* found the end of the parent dir */
      return w_string_piece(s_, end - s_);
    }
  }
  return nullptr;
}

w_string_piece w_string_piece::baseName() const {
  for (auto end = e_; end >= s_; --end) {
    if (*end == WATCHMAN_DIR_SEP) {
      /* found the end of the parent dir */
      return w_string_piece(s_, end - s_);
    }
  }

  return *this;
}

bool w_string_piece::operator==(w_string_piece other) const {
  if (s_ == other.s_ && e_ == other.e_) {
    return true;
  }
  if (size() != other.size()) {
    return false;
  }
  return memcmp(data(), other.data(), size()) == 0;
}

bool w_string_piece::startsWith(w_string_piece prefix) const {
  if (prefix.size() > size()) {
    return false;
  }
  return memcmp(data(), prefix.data(), prefix.size()) == 0;
}

bool w_string_piece::startsWithCaseInsensitive(w_string_piece prefix) const{
  if (prefix.size() > size()) {
    return false;
  }

  auto me = s_;
  auto pref = prefix.s_;

  while (pref < prefix.e_) {
    if (tolower((uint8_t)*me) != tolower((uint8_t)*pref)) {
      return false;
    }
    ++pref;
    ++me;
  }
  return true;
}

// string

w_string::w_string() {}

w_string::w_string(std::nullptr_t) {}

w_string::~w_string() {
  if (str_) {
    w_string_delref(str_);
  }
}

w_string::w_string(w_string_t* str, bool addRef) : str_(str) {
  if (str_ && addRef) {
    w_string_addref(str_);
  }
}

w_string::w_string(const w_string& other) : str_(other.str_) {
  if (str_) {
    w_string_addref(str_);
  }
}

w_string& w_string::operator=(const w_string& other) {
  if (&other == this) {
    return *this;
  }

  reset();
  if (str_) {
    w_string_delref(str_);
  }
  str_ = other.str_;
  if (str_) {
    w_string_addref(str_);
  }

  return *this;
}

w_string::w_string(w_string&& other) noexcept : str_(other.str_) {
  other.str_ = nullptr;
}

w_string& w_string::operator=(w_string&& other) {
  if (&other == this) {
    return *this;
  }
  reset();
  str_ = other.str_;
  other.str_ = nullptr;
  return *this;
}

void w_string::reset() {
  if (str_) {
    w_string_delref(str_);
    str_ = nullptr;
  }
}

w_string_t *w_string::release() {
  auto res = str_;
  str_ = nullptr;
  return res;
}

w_string::w_string(const char* buf, uint32_t len, w_string_type_t stringType)
    : w_string(w_string_new_len_typed(buf, len, stringType), false) {}

w_string::w_string(const char* buf, w_string_type_t stringType)
    : w_string(
          w_string_new_len_typed(buf, strlen_uint32(buf), stringType),
          false) {}

w_string w_string::dirName() const {
  return w_string(w_string_dirname(str_), false);
}

w_string w_string::baseName() const {
  return w_string(w_string_basename(str_), false);
}

w_string w_string::suffix() const {
  return w_string(w_string_suffix(str_), false);
}

w_string w_string::asNullTerminated() const {
  if (w_string_is_null_terminated(str_)) {
    return *this;
  }

  return w_string(str_->buf, str_->len, str_->type);
}

void w_string::makeNullTerminated() {
  if (w_string_is_null_terminated(str_)) {
    return;
  }

  *this = asNullTerminated();
}

const char* w_string::c_str() const {
  if (!w_string_is_null_terminated(str_)) {
    throw std::runtime_error(
        "string is not NULL terminated, use asNullTerminated() or makeNullTerminated()!");
  }
  return str_->buf;
}

bool w_string::operator<(const w_string& other) const {
  return w_string_compare(str_, other.str_) < 0;
}

bool w_string::operator==(const w_string& other) const {
  return w_string_equal(str_, other.str_);
}

bool w_string::operator!=(const w_string& other) const {
  return !(*this == other);
}

w_string w_string::pathCat(std::initializer_list<w_string_piece> elems) {
  uint32_t length = 0;
  w_string_t *s;
  char *buf;

  for (auto &p : elems) {
    length += p.size() + 1;
  }

  s = (w_string_t*)(new char[sizeof(*s) + length]);
  new (s) watchman_string();

  s->refcnt = 1;
  buf = (char *)(s + 1);
  s->buf = buf;

  for (auto &p : elems) {
    if (p.size() == 0 && buf == s->buf) {
      // Skip leading empty strings
      continue;
    }
    if (buf != s->buf) {
      *buf = WATCHMAN_DIR_SEP;
      ++buf;
    }
    memcpy(buf, p.data(), p.size());
    buf += p.size();
  }
  *buf = 0;
  s->len = buf - s->buf;

  return w_string(s, false);
}

uint32_t w_string_compute_hval(w_string_t *str) {
  str->_hval = w_hash_bytes(str->buf, str->len, 0);
  str->hval_computed = 1;
  return str->_hval;
}

/** An optimization to avoid heap allocations during a lookup, this function
 * creates a string object on the stack.  This object does not own the memory
 * that it references, so it is the responsibility of the caller
 * to ensure that that memory is live for the duration of use of this string.
 * It is therefore invalid to add a reference or take a slice of this stack
 * string as the lifetime guarantees are not upheld. */
void w_string_new_len_typed_stack(w_string_t *into, const char *str,
                                  uint32_t len, w_string_type_t type) {
  into->refcnt = 1;
  into->slice = NULL;
  into->len = len;
  into->buf = str;
  into->hval_computed = 0;
  into->type = type;
}

w_string_t *w_string_slice(w_string_t *str, uint32_t start, uint32_t len)
{
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

  // Can't just new w_string_t because the delref has to call delete[]
  // in most cases.
  auto slice = (w_string_t*)(new char[sizeof(w_string_t)]);
  new (slice) watchman_string();

  slice->refcnt = 1;
  slice->len = len;
  slice->buf = str->buf + start;
  slice->slice = str;
  slice->type = str->type;

  w_string_addref(str);
  return slice;
}

w_string w_string::slice(uint32_t start, uint32_t len) const {
  return w_string(w_string_slice(str_, start, len), false);
}

uint32_t strlen_uint32(const char *str) {
  size_t slen = strlen(str);
  if (slen > UINT32_MAX) {
    w_log(W_LOG_FATAL, "string of length %" PRIsize_t " is too damned long\n",
        slen);
  }

  return (uint32_t)slen;
}

// Returns the memory size required to embed str into some other struct
uint32_t w_string_embedded_size(w_string_t *str) {
  return sizeof(*str) + str->len + 1;
}

// Copies src -> dest.  dest is assumed to be some memory of at least
// w_string_embedded_size().
void w_string_embedded_copy(w_string_t *dest, w_string_t *src) {
  char *buf;
  dest->refcnt = 1;
  dest->_hval = src->_hval;
  dest->hval_computed = src->hval_computed;
  dest->len = src->len;
  dest->slice = NULL;

  buf = (char*)(dest + 1);
  memcpy(buf, src->buf, src->len);
  buf[dest->len] = 0;

  dest->buf = buf;
}

watchman_string::watchman_string()
    : refcnt(0),
      len(0),
      slice(nullptr),
      buf(nullptr),
      type(W_STRING_BYTE),
      hval_computed(0) {}

watchman_string::~watchman_string() {
  if (slice) {
    w_string_delref(slice);
  }
}

w_string_t *w_string_new_len_with_refcnt_typed(const char* str,
    uint32_t len, long refcnt, w_string_type_t type) {

  w_string_t *s;
  char *buf;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = refcnt;
  s->len = len;
  buf = (char*)(s + 1);
  memcpy(buf, str, len);
  buf[len] = 0;
  s->buf = buf;
  s->type = type;

  return s;
}

w_string_t *w_string_new_len_typed(const char *str, uint32_t len,
    w_string_type_t type) {
  return w_string_new_len_with_refcnt_typed(str, len, 1, type);
}

w_string_t *w_string_new_len_no_ref_typed(const char *str, uint32_t len,
    w_string_type_t type) {
  return w_string_new_len_with_refcnt_typed(str, len, 0, type);
}

w_string_t *w_string_new_typed(const char *str, w_string_type_t type) {
  return w_string_new_len_typed(str, strlen_uint32(str), type);
}

#ifdef _WIN32
w_string_t *w_string_new_wchar_typed(WCHAR *str, int len,
    w_string_type_t type) {
  char buf[WATCHMAN_NAME_MAX];
  int res;

  if (len == 0) {
    return w_string_new_typed("", type);
  }

  res = WideCharToMultiByte(CP_UTF8, 0, str, len, buf, sizeof(buf), NULL, NULL);
  if (res == 0) {
    char msgbuf[1024];
    DWORD err = GetLastError();
    FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      msgbuf, sizeof(msgbuf)-1, NULL);
    w_log(W_LOG_ERR, "WideCharToMultiByte failed: 0x%x %s\n", err, msgbuf);
    return NULL;
  }

  buf[res] = 0;
  return w_string_new_typed(buf, type);
}

#endif

w_string w_string::printf(WATCHMAN_FMT_STRING(const char* format), ...) {
  w_string_t *s;
  int len;
  char *buf;
  va_list args;

  va_start(args, format);
  // Get the length needed
  len = vsnprintf(nullptr, 0, format, args);
  va_end(args);

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  if (!s) {
    perror("no memory available");
    abort();
  }

  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = (char*)(s + 1);
  va_start(args, format);
  vsnprintf(buf, len + 1, format, args);
  va_end(args);
  s->buf = buf;

  return w_string(s, false);
}

/* return a reference to a lowercased version of a string */
w_string_t *w_string_dup_lower(w_string_t *str)
{
  bool is_lower = true;
  char *buf;
  uint32_t i;
  w_string_t *s;

  for (i = 0; i < str->len; i++) {
    if (tolower((uint8_t)str->buf[i]) != str->buf[i]) {
      is_lower = false;
      break;
    }
  }

  if (is_lower) {
    w_string_addref(str);
    return str;
  }

  /* need to make a lowercase version */

  s = (w_string_t*)(new char[sizeof(*s) + str->len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = str->len;
  buf = (char*)(s + 1);
  for (i = 0; i < str->len; i++) {
    buf[i] = (char)tolower((uint8_t)str->buf[i]);
  }
  buf[str->len] = 0;
  s->buf = buf;

  return s;
}

/* make a lowercased copy of string */
w_string_t *w_string_new_lower_typed(const char *str,
    w_string_type_t type)
{
  w_string_t *s;
  uint32_t len = strlen_uint32(str);
  char *buf;
  uint32_t i;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = (char*)(s + 1);
  // TODO: optionally use ICU
  for (i = 0; i < len; i++) {
    buf[i] = (char)tolower((uint8_t)str[i]);
  }
  buf[len] = 0;
  s->buf = buf;
  s->type = type;

  return s;
}

void w_string_addref(w_string_t *str)
{
  ++str->refcnt;
}

void w_string_delref(w_string_t *str)
{
  if (--str->refcnt != 0) {
    return;
  }
  // Call the destructor.  We can't use regular delete because
  // we allocated using operator new[], and we can't use delete[]
  // directly either because the type doesn't match what we allocated.
  str->~w_string_t();
  // Release the raw memory.
  delete[](char*) str;
}

int w_string_compare(const w_string_t *a, const w_string_t *b)
{
  int res;
  if (a == b) return 0;
  if (a->len < b->len) {
    res = memcmp(a->buf, b->buf, a->len);
    return res == 0 ? -1 : res;
  } else if (a->len > b->len) {
    res = memcmp(a->buf, b->buf, b->len);
    return res == 0 ? +1 : res;
  }
  return memcmp(a->buf, b->buf, a->len);
}

bool w_string_equal_cstring(const w_string_t *a, const char *b)
{
  uint32_t blen = strlen_uint32(b);
  if (a->len != blen) return false;
  return memcmp(a->buf, b, a->len) == 0 ? true : false;
}

bool w_string_equal(const w_string_t *a, const w_string_t *b)
{
  if (a == b) return true;
  if (a == nullptr || b == nullptr) return false;
  if (a->len != b->len) return false;
  if (a->hval_computed && b->hval_computed && a->_hval != b->_hval) {
    return false;
  }
  return memcmp(a->buf, b->buf, a->len) == 0 ? true : false;
}

bool w_string_equal_caseless(const w_string_t *a, const w_string_t *b)
{
  uint32_t i;
  if (a == b) return true;
  if (a->len != b->len) return false;
  for (i = 0; i < a->len; i++) {
    if (tolower((uint8_t)a->buf[i]) != tolower((uint8_t)b->buf[i])) {
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
    if (str->buf[end] == WATCHMAN_DIR_SEP) {
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
    if (tolower((uint8_t)str->buf[base + i]) != suffix->buf[i]) {
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
        *buf = (char)tolower((uint8_t)str->buf[end]);
        end++;
        buf++;
      }
      *buf = '\0';
      return w_string_new_typed(name_buf, str->type);
    }

    if (str->buf[end] == WATCHMAN_DIR_SEP) {
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

bool w_string_startswith_caseless(w_string_t *str, w_string_t *prefix)
{
  size_t i;

  if (prefix->len > str->len) {
    return false;
  }
  for (i = 0; i < prefix->len; i++) {
    if (tolower((uint8_t)str->buf[i]) != tolower((uint8_t)prefix->buf[i])) {
      return false;
    }
  }
  return true;
}

bool w_string_contains_cstr_len(
    const w_string_t* str,
    const char* needle,
    uint32_t nlen) {
#if HAVE_MEMMEM
  return memmem(str->buf, str->len, needle, nlen) != NULL;
#else
  // Most likely only for Windows.
  // Inspired by http://stackoverflow.com/a/24000056/149111
  const char *haystack = str->buf;
  uint32_t hlen = str->len;
  const char *limit;

  if (nlen == 0 || hlen < nlen) {
    return false;
  }

  limit = haystack + hlen - nlen + 1;
  while ((haystack = (const char*)memchr(
              haystack, needle[0], limit - haystack)) != NULL) {
    if (memcmp(haystack, needle, nlen) == 0) {
      return true;
    }
    haystack++;
  }
  return false;
#endif
}

w_string_t *w_string_canon_path(w_string_t *str)
{
  int end;
  int trim = 0;

  for (end = str->len - 1;
      end >= 0 && str->buf[end] == WATCHMAN_DIR_SEP; end--) {
    trim++;
  }
  if (trim) {
    return w_string_slice(str, 0, str->len - trim);
  }
  w_string_addref(str);
  return str;
}

#ifdef _WIN32
#define WRONG_SEP '/'
#else
#define WRONG_SEP '\\'
#endif

// Normalize directory separators to match the platform.
// Also trims any trailing directory separators
w_string_t *w_string_normalize_separators(w_string_t *str, char target_sep) {
  w_string_t *s;
  char *buf;
  uint32_t i, len;

  len = str->len;

  if (len == 0) {
    w_string_addref(str);
    return str;
  }

  // This doesn't do any special UNC or path len escape prefix handling
  // on windows.  We don't currently use it in a way that would require it.

  // Trim any trailing dir seps
  while (len > 0) {
    if (str->buf[len-1] == '/' || str->buf[len-1] == '\\') {
      --len;
    } else {
      break;
    }
  }

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = (char*)(s + 1);

  for (i = 0; i < len; i++) {
    if (str->buf[i] == '/' || str->buf[i] == '\\') {
      buf[i] = target_sep;
    } else {
      buf[i] = str->buf[i];
    }
  }
  buf[len] = 0;
  s->buf = buf;

  return s;
}

void w_string_in_place_normalize_separators(w_string_t **str, char target_sep) {
  w_string_t *norm = w_string_normalize_separators(*str, target_sep);
  w_string_delref(*str);
  *str = norm;
}

// Compute the basename of path, return that as a string
w_string_t *w_string_new_basename_typed(const char *path,
    w_string_type_t type) {
  const char *base;
  base = path + strlen(path);
  while (base > path && base[-1] != WATCHMAN_DIR_SEP) {
    base--;
  }
  return w_string_new_typed(base, type);
}

w_string_t *w_string_basename(w_string_t *str)
{
  int end;

  /* can't use libc strXXX functions because we may be operating
   * on a slice */
  for (end = str->len - 1; end >= 0; end--) {
    if (str->buf[end] == WATCHMAN_DIR_SEP) {
      /* found the end of the parent dir */
      return w_string_slice(str, end + 1, str->len - (end + 1));
    }
  }

  w_string_addref(str);
  return str;
}

w_string_t *w_string_path_cat(w_string_t *parent, w_string_t *rhs)
{
  w_string_t *s;
  int len;
  char *buf;

  if (rhs->len == 0) {
    w_string_addref(parent);
    return parent;
  }

  len = parent->len + rhs->len + 1;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = (char*)(s + 1);
  memcpy(buf, parent->buf, parent->len);
  buf[parent->len] = WATCHMAN_DIR_SEP;
  memcpy(buf + parent->len + 1, rhs->buf, rhs->len);
  buf[parent->len + 1 + rhs->len] = '\0';
  s->buf = buf;
  s->type = parent->type;

  return s;
}

w_string_t *w_string_path_cat_cstr(w_string_t *parent, const char *rhs) {
  return w_string_path_cat_cstr_len(parent, rhs, strlen_uint32(rhs));
}

w_string_t *w_string_path_cat_cstr_len(w_string_t *parent, const char *rhs,
                                       uint32_t rhs_len) {
  w_string_t *s;
  int len;
  char *buf;

  if (rhs_len == 0) {
    w_string_addref(parent);
    return parent;
  }

  len = parent->len + rhs_len + 1;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = (char*)(s + 1);
  memcpy(buf, parent->buf, parent->len);
  buf[parent->len] = WATCHMAN_DIR_SEP;
  memcpy(buf + parent->len + 1, rhs, rhs_len);
  buf[parent->len + 1 + rhs_len] = '\0';
  s->buf = buf;
  s->type = parent->type;

  return s;
}

w_string_t* w_dir_path_cat_cstr(
    const struct watchman_dir* dir,
    const char* extra) {
  return w_dir_path_cat_cstr_len(dir, extra, strlen_uint32(extra));
}

w_string_t* w_dir_path_cat_cstr_len(
    const struct watchman_dir* dir,
    const char* extra,
    uint32_t extra_len) {
  uint32_t length = 0;
  const struct watchman_dir* d;
  w_string_t *s;
  char *buf, *end;

  if (extra && extra_len) {
    length = extra_len + 1 /* separator */;
  }
  for (d = dir; d; d = d->parent) {
    length += d->name.size() + 1 /* separator OR final NUL terminator */;
  }

  s = (w_string_t*)(new char[sizeof(*s) + length]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = length - 1;
  buf = (char *)(s + 1);
  end = buf + s->len;

  *end = 0;
  if (extra && extra_len) {
    end -= extra_len;
    memcpy(end, extra, extra_len);
  }
  for (d = dir; d; d = d->parent) {
    if (d != dir || (extra && extra_len)) {
      --end;
      *end = WATCHMAN_DIR_SEP;
    }
    end -= d->name.size();
    memcpy(end, d->name.data(), d->name.size());
  }

  s->buf = buf;
  return s;
}

w_string_t* w_dir_copy_full_path(const struct watchman_dir* dir) {
  return w_dir_path_cat_cstr_len(dir, NULL, 0);
}

w_string_t* w_dir_path_cat_str(
    const struct watchman_dir* dir,
    w_string_t* str) {
  return w_dir_path_cat_cstr_len(dir, str->buf, str->len);
}

char *w_string_dup_buf(const w_string_t *str)
{
  char *buf;

  buf = (char*)malloc(str->len + 1);
  if (!buf) {
    return NULL;
  }

  memcpy(buf, str->buf, str->len);
  buf[str->len] = 0;

  return buf;
}


// Given a string, return a shell-escaped copy
w_string_t *w_string_shell_escape(const w_string_t *str)
{
  // Worst case expansion for a char is 4x, plus quoting either end
  uint32_t len = 2 + (str->len * 4);
  w_string_t *s;
  char *buf;
  const char *src, *end;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
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
  s->len = (uint32_t)(buf - s->buf);
  s->type = str->type;

  return s;
}

bool w_string_is_known_unicode(w_string_t *str) {
  return str->type == W_STRING_UNICODE;
}

bool w_string_is_null_terminated(w_string_t *str) {
  return !str->slice ||
    (str->buf + str->len == str->slice->buf + str->slice->len &&
     w_string_is_null_terminated(str->slice));
}

size_t w_string_strlen(w_string_t *str) {
  return str->len;
}

bool w_string_path_is_absolute(const w_string_t *str) {
  return w_is_path_absolute_cstr_len(str->buf, str->len);
}

bool w_is_path_absolute_cstr(const char *path) {
  return w_is_path_absolute_cstr_len(path, strlen_uint32(path));
}

bool w_is_path_absolute_cstr_len(const char *path, uint32_t len) {
#ifdef _WIN32
  char drive_letter;

  if (len <= 2) {
    return false;
  }

  // "\something"
  if (is_slash(path[0])) {
    // "\\something" is absolute, "\something" is relative to the current
    // dir of the current drive, whatever that may be, for a given process
    return is_slash(path[1]);
  }

  drive_letter = (char)tolower(path[0]);
  // "C:something"
  if (drive_letter >= 'a' && drive_letter <= 'z' && path[1] == ':') {
    // "C:\something" is absolute, but "C:something" is relative to
    // the current dir on the C drive(!)
    return is_slash(path[2]);
  }
  // we could check for things like NUL:, COM: and so on here.
  // While those are technically absolute names, we can't watch them, so
  // we don't consider them absolute for the purposes of checking whether
  // the path is a valid watchable root
  return false;
#else
  return len > 0 && path[0] == '/';
#endif
}

/* vim:ts=2:sw=2:et:
 */
