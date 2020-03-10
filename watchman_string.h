/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_STRING_H
#define WATCHMAN_STRING_H

#include "watchman_system.h"

#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <memory>
#include "watchman_log.h"
#ifdef _WIN32
#include <string>
#endif
#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <stdexcept>
#include <vector>

struct watchman_string;
typedef struct watchman_string w_string_t;
class w_string_piece;

typedef enum {
  W_STRING_BYTE,
  W_STRING_UNICODE,
  W_STRING_MIXED
} w_string_type_t;

struct watchman_string {
  std::atomic<long> refcnt;
  uint32_t _hval;
  uint32_t len;
  w_string_type_t type : 3;
  unsigned hval_computed : 1;

  // This holds the character data.  This is a variable
  // sized member and we have to specify at least 1 byte
  // for the compiler to accept this.
  // This must be the last element of this struct.
  const char buf[1];

  inline watchman_string()
      : refcnt(0), len(0), type(W_STRING_BYTE), hval_computed(0), buf{0} {}
};

uint32_t w_string_compute_hval(w_string_t* str);

static inline uint32_t w_string_hval(w_string_t* str) {
  if (str->hval_computed) {
    return str->_hval;
  }
  return w_string_compute_hval(str);
}

w_string_piece w_string_canon_path(w_string_t* str);
int w_string_compare(const w_string_t* a, const w_string_t* b);
bool w_string_contains_cstr_len(
    const w_string_t* str,
    const char* needle,
    uint32_t nlen);

bool w_string_equal(const w_string_t* a, const w_string_t* b);
bool w_string_equal_cstring(const w_string_t* a, const char* b);

bool w_string_path_is_absolute(const w_string_t* str);

bool w_string_startswith(w_string_t* str, w_string_t* prefix);
bool w_string_startswith_caseless(w_string_t* str, w_string_t* prefix);

bool w_string_is_known_unicode(w_string_t* str);
bool w_string_is_null_terminated(w_string_t* str);

uint32_t strlen_uint32(const char* str);

bool w_is_path_absolute_cstr(const char* path);
bool w_is_path_absolute_cstr_len(const char* path, uint32_t len);

inline bool is_slash(char c) {
  return c == '/'
#ifdef _WIN32
      || c == '\\'
#endif
      ;
}

class w_string;

/** Represents a view over some externally managed string storage.
 * It is simply a pair of pointers that define the start and end
 * of the valid region. */
class w_string_piece {
  const char *s_, *e_;

 public:
  w_string_piece();
  /* implicit */ w_string_piece(std::nullptr_t);

  inline /* implicit */ w_string_piece(w_string_t* str)
      : s_(str->buf), e_(str->buf + str->len) {}

  /** Construct from a string-like object */
  template <
      typename String,
      typename std::enable_if<
          std::is_class<String>::value &&
              !std::is_same<String, w_string>::value,
          int>::type = 0>
  inline /* implicit */ w_string_piece(const String& str)
      : s_(str.data()), e_(str.data() + str.size()) {}

  /** Construct from w_string.  This is almost the same as
   * the string like object constructor above, but we need a nullptr check
   */
  template <
      typename String,
      typename std::enable_if<std::is_same<String, w_string>::value, int>::
          type = 0>
  inline /* implicit */ w_string_piece(const String& str) {
    if (!str) {
      s_ = nullptr;
      e_ = nullptr;
    } else {
      s_ = str.data();
      e_ = str.data() + str.size();
    }
  }

  inline /* implicit */ w_string_piece(const char* cstr)
      : s_(cstr), e_(cstr + strlen(cstr)) {}

  /* implicit */ inline w_string_piece(const char* cstr, size_t len)
      : s_(cstr), e_(cstr + len) {}

  w_string_piece(const w_string_piece& other) = default;
  w_string_piece& operator=(const w_string_piece& other) = default;
  w_string_piece(w_string_piece&& other) noexcept;

  inline const char* data() const {
    return s_;
  }

  inline bool empty() const {
    return e_ == s_;
  }

  inline size_t size() const {
    return e_ - s_;
  }

  const char& operator[](size_t i) const {
    return s_[i];
  }

  /** move the start of the string by n characters, stripping off that prefix */
  void advance(size_t n) {
    if (n > size()) {
      throw std::range_error("index out of range");
    }
    s_ += n;
  }

  /** Return a copy of the string as a w_string */
  w_string asWString(w_string_type_t stringType = W_STRING_BYTE) const;

  /** Return a lowercased copy of the string */
  w_string asLowerCase(w_string_type_t stringType = W_STRING_BYTE) const;

  /** Return a lowercased copy of the suffix */
  w_string asLowerCaseSuffix(w_string_type_t stringType = W_STRING_BYTE) const;

  /** Return a UTF-8-clean copy of the string */
  w_string asUTF8Clean() const;

  /** Returns true if the filename suffix of this string matches
   * the provided suffix, which must be lower cased.
   * This string piece lower cased and matched against suffix */
  bool hasSuffix(w_string_piece suffix) const;

  bool pathIsAbsolute() const;
  bool pathIsEqual(w_string_piece other) const;
  w_string_piece dirName() const;
  w_string_piece baseName() const;
  w_string_piece suffix() const;

  /** Split the string by delimiter and emit to the provided vector */
  template <typename Vector>
  void split(Vector& result, char delim) const {
    const char* begin = s_;
    const char* it = begin;
    while (it != e_) {
      if (*it == delim) {
        result.emplace_back(begin, it - begin);
        begin = ++it;
        continue;
      }
      ++it;
    }

    if (begin != e_) {
      result.emplace_back(begin, e_ - begin);
    }
  }

  operator folly::StringPiece() const {
    return folly::StringPiece(s_, e_);
  }

  bool operator==(w_string_piece other) const;
  bool operator!=(w_string_piece other) const;
  bool operator<(w_string_piece other) const;

  bool startsWith(w_string_piece prefix) const;
  bool startsWithCaseInsensitive(w_string_piece prefix) const;

  // Compute a hash value for this piece
  uint32_t hashValue() const;

#ifdef _WIN32
  // Returns a wide character representation of the piece
  std::wstring asWideUNC() const;
#endif
};

bool w_string_equal_caseless(w_string_piece a, w_string_piece b);

/** A smart pointer class for tracking w_string_t instances */
class w_string {
 public:
  /** Initialize a nullptr */
  w_string();
  /* implicit */ w_string(std::nullptr_t);

  /** Make a new string from some bytes and a type */
  w_string(
      const char* buf,
      size_t len,
      w_string_type_t stringType = W_STRING_BYTE);
  /* implicit */ w_string(
      const char* buf,
      w_string_type_t stringType = W_STRING_BYTE);

#ifdef _WIN32
  /** Convert a wide character path to utf-8 and return it as a w_string.
   * This constructor is intended only for path names and not as a general
   * WCHAR -> w_string constructor; it will always apply path mapping
   * logic to the result and may mangle non-pathname strings if they
   * are passed to it. */
  w_string(const WCHAR* wpath, size_t len);
#endif

  /** Initialize, taking a ref on w_string_t */
  /* implicit */ w_string(w_string_t* str, bool addRef = true);
  /** Release the string reference when we fall out of scope */
  ~w_string();
  /** Copying adds a ref */
  w_string(const w_string& other);
  w_string& operator=(const w_string& other);
  /** Moving steals a ref */
  w_string(w_string&& other) noexcept;
  w_string& operator=(w_string&& other);

  /** Stop tracking the underlying string object, decrementing
   * the reference count. */
  void reset();

  /** Stop tracking the underlying string object, returning the
   * reference to the caller.  The caller is responsible for
   * decrementing the refcount */
  w_string_t* release();

  operator w_string_t*() const {
    return str_;
  }

  operator w_string_piece() const {
    return piece();
  }

  operator folly::StringPiece() const {
    if (str_ == nullptr) {
      return folly::StringPiece();
    }
    return folly::StringPiece(data(), size());
  }

  inline w_string_piece piece() const {
    if (str_ == nullptr) {
      return w_string_piece();
    }
    return w_string_piece(data(), size());
  }

  explicit operator bool() const {
    return str_ != nullptr;
  }

  bool operator==(const w_string& other) const;
  bool operator!=(const w_string& other) const;
  bool operator<(const w_string& other) const;

  /** path concatenation
   * Pass in a list of w_string_pieces to join them all similarly to
   * the python os.path.join() function. */
  static w_string pathCat(std::initializer_list<w_string_piece> elems);

  /** Similar to asprintf, but returns a w_string */
  static w_string vprintf(const char* format, va_list ap);

  /** build a w_string by concatenating the string formatted representation
   * of each of the supplied arguments */
  template <typename... Args>
  static w_string build(Args&&... args) {
    static const char format_str[] = "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}";
    static_assert(
        sizeof...(args) <= sizeof(format_str) / 2,
        "too many args passed to w_string::build");
    return w_string::format(
        fmt::string_view(format_str, sizeof...(args) * 2),
        std::forward<Args>(args)...);
  }

  /** Construct a new string using the `fmt` formatting library.
   * Syntax: https://fmt.dev/latest/syntax.html
   */
  template <typename... Args>
  static w_string format(fmt::string_view format_str, Args&&... args) {
    auto size = fmt::formatted_size(format_str, args...);

    w_string_t* s = (w_string_t*)(new char[sizeof(*s) + size + 1]);
    new (s) watchman_string();

    {
      // in case format_to throws
      SCOPE_FAIL {
        delete[](char*) s;
      };

      s->refcnt = 1;
      s->len = uint32_t(size);

      auto mut_buf = const_cast<char*>(s->buf);
      fmt::format_to(mut_buf, format_str, args...);

      mut_buf[s->len] = 0;
    }

    return w_string(s, false);
  }

  /** Return a possibly new version of this string that has its separators
   * normalized to unix slashes */
  w_string normalizeSeparators(char targetSeparator = '/') const;

  inline void ensureNotNull() const {
    if (!str_) {
      throw std::runtime_error("failed assertion w_string::ensureNotNull");
    }
  }

  /** Returns a pointer to a null terminated c-string. */
  const char* c_str() const {
    return data();
  }
  const char* data() const {
    ensureNotNull();
    return str_->buf;
  }

  bool empty() const {
    if (str_) {
      return str_->len == 0;
    }
    return true;
  }

  size_t size() const {
    ensureNotNull();
    return str_->len;
  }

  w_string_type_t type() const {
    ensureNotNull();
    return str_->type;
  }

  /** Returns the directory component of the string, assuming a path string */
  w_string dirName() const;
  /** Returns the file name component of the string, assuming a path string */
  w_string baseName() const;
  /** Returns the filename suffix of a path string */
  w_string asLowerCaseSuffix() const;

 private:
  w_string_t* str_{nullptr};
};

/** Allow w_string to act as a key in unordered_(map|set) */
namespace std {
template <>
struct hash<w_string> {
  std::size_t operator()(w_string const& str) const {
    return w_string_hval(str);
  }
};
template <>
struct hash<w_string_piece> {
  std::size_t operator()(w_string_piece const& str) const {
    return str.hashValue();
  }
};
} // namespace std

// Streaming operators for logging and printing
std::ostream& operator<<(std::ostream& stream, const w_string& a);
std::ostream& operator<<(std::ostream& stream, const w_string_piece& a);

// Allow formatting w_string and w_string_piece
namespace fmt {
template <>
struct formatter<w_string> {
  template <typename ParseContext>
  constexpr auto parse(ParseContext& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const w_string& s, FormatContext& ctx) {
    return format_to(ctx.out(), "{}", folly::StringPiece(s));
  }
};

template <>
struct formatter<w_string_piece> {
  template <typename ParseContext>
  constexpr auto parse(ParseContext& ctx) {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const w_string_piece& s, FormatContext& ctx) {
    return format_to(ctx.out(), "{}", folly::StringPiece(s));
  }
};
} // namespace fmt

#endif

/* vim:ts=2:sw=2:et:
 */
