/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <stdarg.h>
#include <new>
#include <ostream>
#include <stdexcept>
#include "watchman/thirdparty/jansson/utf.h"
#include "watchman/watchman_hash.h"
#include "watchman/watchman_string.h"

static void w_string_addref(w_string_t* str);
static void w_string_delref(w_string_t* str);
static w_string_t*
w_string_new_len_typed(const char* str, uint32_t len, w_string_type_t type);

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

w_string w_string_piece::asLowerCase(w_string_type_t stringType) const {
  char* buf;
  w_string_t* s;

  /* need to make a lowercase version */
  s = (w_string_t*)(new char[sizeof(*s) + size() + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = size();
  buf = const_cast<char*>(s->buf);
  s->type = stringType;

  auto cursor = s_;
  while (cursor < e_) {
    *buf = (char)tolower((uint8_t)*cursor);
    ++cursor;
    ++buf;
  }
  *buf = 0;

  return w_string(s, false);
}

w_string w_string_piece::asLowerCaseSuffix(w_string_type_t stringType) const {
  char* buf;
  w_string_t* s;

  w_string_piece suffixPiece = this->suffix();
  if (suffixPiece == nullptr) {
    return nullptr;
  }

  /* need to make a lowercase version */
  s = (w_string_t*)(new char[sizeof(*s) + suffixPiece.size() + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = suffixPiece.size();
  buf = const_cast<char*>(s->buf);
  s->type = stringType;

  auto cursor = suffixPiece.s_;
  while (cursor < suffixPiece.e_) {
    *buf = (char)tolower((uint8_t)*cursor);
    ++cursor;
    ++buf;
  }
  *buf = 0;

  return w_string(s, false);
}

w_string w_string_piece::asUTF8Clean() const {
  w_string s(s_, e_ - s_, W_STRING_UNICODE);
  utf8_fix_string(const_cast<char*>(s.data()), s.size());
  return s;
}

bool w_string_piece::pathIsAbsolute() const {
  return w_is_path_absolute_cstr_len(data(), size());
}

/** Compares two path strings.
 * They are equal if the case of each character matches.
 * Directory separator slashes are normalized such that
 * \ and / are considered equal. */
bool w_string_piece::pathIsEqual(w_string_piece other) const {
#ifdef _WIN32
  if (size() != other.size()) {
    return false;
  }

  auto A = data();
  auto B = other.data();

  auto end = A + size();
  for (; A < end; ++A, ++B) {
    if (*A == *B) {
      continue;
    }
    if (A == data()) {
      // This is a bit awful, but msys and other similar software
      // can set the cwd to a lowercase drive letter.  Since we
      // can't ever watch at a level higher than drive letters,
      // we really shouldn't care about a case difference there
      // so we relax the strictness of the check here.
      // This case only triggers for the first character of the
      // path.  Paths evaluated with this method are always
      // absolute.  In theory, we should also do something
      // reasonable for UNC paths, but folks shouldn't be
      // watching those with watchman anyway.
      if (tolower(*A) == tolower(*B)) {
        continue;
      }
    }

    if (is_slash(*A) && is_slash(*B)) {
      continue;
    }
    return false;
  }
  return true;
#else
  return *this == other;
#endif
}

w_string_piece w_string_piece::dirName() const {
  if (e_ == s_) {
    return nullptr;
  }
  for (auto end = e_ - 1; end >= s_; --end) {
    if (is_slash(*end)) {
      /* found the end of the parent dir */
#ifdef _WIN32
      if (end > s_ && end[-1] == ':') {
        // Special case for "C:\"; we want to keep the
        // trailing slash for this case so that we continue
        // to consider it an absolute path
        return w_string_piece(s_, 1 + end - s_);
      }
#endif
      return w_string_piece(s_, end - s_);
    }
  }
  return nullptr;
}

w_string_piece w_string_piece::baseName() const {
  if (e_ == s_) {
    return *this;
  }
  for (auto end = e_ - 1; end >= s_; --end) {
    if (is_slash(*end)) {
      /* found the end of the parent dir */
#ifdef _WIN32
      if (end == e_ && end > s_ && end[-1] == ':') {
        // Special case for "C:\"; we want the baseName to
        // be this same component so that we continue
        // to consider it an absolute path
        return *this;
      }
#endif
      return w_string_piece(end + 1, e_ - (end + 1));
    }
  }

  return *this;
}

w_string_piece w_string_piece::suffix() const {
  if (e_ == s_) {
    return nullptr;
  }
  for (auto end = e_ - 1; end >= s_; --end) {
    if (is_slash(*end)) {
      return nullptr;
    }
    if (*end == '.') {
      return w_string_piece(end + 1, e_ - (end + 1));
    }
  }
  return nullptr;
}

bool w_string_piece::operator<(w_string_piece other) const {
  int res;
  if (size() < other.size()) {
    res = memcmp(data(), other.data(), size());
    return (res == 0 ? -1 : res) < 0;
  } else if (size() > other.size()) {
    res = memcmp(data(), other.data(), other.size());
    return (res == 0 ? +1 : res) < 0;
  }
  return memcmp(data(), other.data(), size()) < 0;
}

bool w_string_piece::operator==(w_string_piece other) const {
  if (s_ == other.s_ && e_ == other.e_) {
    return true;
  }
  if (size() != other.size()) {
    return false;
  } else if (size() == 0) {
    return true;
  }
  return memcmp(data(), other.data(), size()) == 0;
}

bool w_string_piece::operator!=(w_string_piece other) const {
  return !operator==(other);
}

bool w_string_piece::startsWith(w_string_piece prefix) const {
  if (prefix.size() > size()) {
    return false;
  }
  return memcmp(data(), prefix.data(), prefix.size()) == 0;
}

bool w_string_piece::startsWithCaseInsensitive(w_string_piece prefix) const {
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

w_string_t* w_string::release() {
  auto res = str_;
  str_ = nullptr;
  return res;
}

static inline uint32_t checked_len(size_t len) {
  if (len > UINT32_MAX) {
    throw std::range_error("string length exceeds UINT32_MAX");
  }
  return len;
}

w_string::w_string(const char* buf, size_t len, w_string_type_t stringType)
    : w_string(
          w_string_new_len_typed(buf, checked_len(len), stringType),
          false) {}

w_string::w_string(const char* buf, w_string_type_t stringType)
    : w_string(
          w_string_new_len_typed(buf, strlen_uint32(buf), stringType),
          false) {}

w_string w_string::dirName() const {
  return w_string_piece(*this).dirName().asWString();
}

w_string w_string::baseName() const {
  return w_string_piece(*this).baseName().asWString();
}

w_string w_string::asLowerCaseSuffix() const {
  ensureNotNull();
  return w_string_piece(*this).asLowerCaseSuffix();
}

w_string w_string::normalizeSeparators(char targetSeparator) const {
  w_string_t* s;
  char* buf;
  uint32_t i, len;

  len = str_->len;

  if (len == 0) {
    return *this;
  }

  // This doesn't do any special UNC or path len escape prefix handling
  // on windows.  We don't currently use it in a way that would require it.

  // Trim any trailing dir seps
  while (len > 0) {
    if (str_->buf[len - 1] == '/' || str_->buf[len - 1] == '\\') {
      --len;
    } else {
      break;
    }
  }

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = const_cast<char*>(s->buf);

  for (i = 0; i < len; i++) {
    if (str_->buf[i] == '/' || str_->buf[i] == '\\') {
      buf[i] = targetSeparator;
    } else {
      buf[i] = str_->buf[i];
    }
  }
  buf[len] = 0;

  return s;
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
  w_string_t* s;
  char* buf;

  for (auto& p : elems) {
    length += p.size() + 1;
  }

  s = (w_string_t*)(new char[sizeof(*s) + length]);
  new (s) watchman_string();

  s->refcnt = 1;
  buf = const_cast<char*>(s->buf);

  for (auto& p : elems) {
    if (p.size() == 0) {
      // Skip empty strings
      continue;
    }
    if (buf != s->buf) {
      *buf = '/';
      ++buf;
    }
    memcpy(buf, p.data(), p.size());
    buf += p.size();
  }
  *buf = 0;
  s->len = buf - s->buf;

  return w_string(s, false);
}

uint32_t w_string_compute_hval(w_string_t* str) {
  str->_hval = w_hash_bytes(str->buf, str->len, 0);
  str->hval_computed = 1;
  return str->_hval;
}

uint32_t w_string_piece::hashValue() const {
  return w_hash_bytes(data(), size(), 0);
}

uint32_t strlen_uint32(const char* str) {
  size_t slen = strlen(str);
  if (slen > UINT32_MAX) {
    throw std::range_error("string length exceeds UINT32_MAX");
  }

  return (uint32_t)slen;
}

static w_string_t*
w_string_new_len_typed(const char* str, uint32_t len, w_string_type_t type) {
  w_string_t* s;
  char* buf;

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = const_cast<char*>(s->buf);
  if (str) {
    memcpy(buf, str, len);
  }
  buf[len] = 0;
  s->type = type;

  return s;
}

w_string w_string::vprintf(const char* format, va_list args) {
  w_string_t* s;
  int len;
  char* buf;
  va_list args_copy;

  va_copy(args_copy, args);
  // Get the length needed
  len = vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);

  s = (w_string_t*)(new char[sizeof(*s) + len + 1]);
  new (s) watchman_string();

  s->refcnt = 1;
  s->len = len;
  buf = const_cast<char*>(s->buf);
  vsnprintf(buf, len + 1, format, args);

  return w_string(s, false);
}

void w_string_addref(w_string_t* str) {
  ++str->refcnt;
}

void w_string_delref(w_string_t* str) {
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

int w_string_compare(const w_string_t* a, const w_string_t* b) {
  int res;
  if (a == b)
    return 0;
  if (a->len < b->len) {
    res = memcmp(a->buf, b->buf, a->len);
    return res == 0 ? -1 : res;
  } else if (a->len > b->len) {
    res = memcmp(a->buf, b->buf, b->len);
    return res == 0 ? +1 : res;
  }
  return memcmp(a->buf, b->buf, a->len);
}

bool w_string_equal_cstring(const w_string_t* a, const char* b) {
  uint32_t blen = strlen_uint32(b);
  if (a->len != blen)
    return false;
  return memcmp(a->buf, b, a->len) == 0 ? true : false;
}

bool w_string_equal(const w_string_t* a, const w_string_t* b) {
  if (a == b)
    return true;
  if (a == nullptr || b == nullptr)
    return false;
  if (a->len != b->len)
    return false;
  if (a->hval_computed && b->hval_computed && a->_hval != b->_hval) {
    return false;
  }
  return memcmp(a->buf, b->buf, a->len) == 0 ? true : false;
}

bool w_string_equal_caseless(w_string_piece a, w_string_piece b) {
  uint32_t i;

  if (a.size() != b.size()) {
    return false;
  }
  for (i = 0; i < a.size(); i++) {
    if (tolower((uint8_t)a[i]) != tolower((uint8_t)b[i])) {
      return false;
    }
  }
  return true;
}

bool w_string_piece::hasSuffix(w_string_piece suffix) const {
  unsigned int base, i;

  if (size() < suffix.size() + 1) {
    return false;
  }

  base = size() - suffix.size();

  if (s_[base - 1] != '.') {
    return false;
  }

  for (i = 0; i < suffix.size(); i++) {
    if (tolower((uint8_t)s_[base + i]) != suffix[i]) {
      return false;
    }
  }

  return true;
}

bool w_string_startswith(w_string_t* str, w_string_t* prefix) {
  if (prefix->len > str->len) {
    return false;
  }
  return memcmp(str->buf, prefix->buf, prefix->len) == 0;
}

bool w_string_startswith_caseless(w_string_t* str, w_string_t* prefix) {
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
  const char* haystack = str->buf;
  uint32_t hlen = str->len;
  const char* limit;

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

w_string_piece w_string_canon_path(w_string_t* str) {
  int end;
  int trim = 0;

  for (end = str->len - 1; end >= 0 && is_slash(str->buf[end]); end--) {
    trim++;
  }
  if (trim) {
    return w_string_piece(str->buf, str->len - trim);
  }
  return w_string_piece(str);
}

bool w_string_is_known_unicode(w_string_t* str) {
  return str->type == W_STRING_UNICODE;
}

bool w_string_path_is_absolute(const w_string_t* str) {
  return w_is_path_absolute_cstr_len(str->buf, str->len);
}

bool w_is_path_absolute_cstr(const char* path) {
  return w_is_path_absolute_cstr_len(path, strlen_uint32(path));
}

bool w_is_path_absolute_cstr_len(const char* path, uint32_t len) {
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

std::ostream& operator<<(std::ostream& stream, const w_string& a) {
  stream << folly::StringPiece(a);
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const w_string_piece& a) {
  stream << folly::StringPiece(a);
  return stream;
}

/* vim:ts=2:sw=2:et:
 */
