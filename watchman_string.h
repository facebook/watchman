/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_STRING_H
#define WATCHMAN_STRING_H

#include <stdbool.h>
#include <stdint.h>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <memory>
#include "watchman_log.h"

struct watchman_string;
typedef struct watchman_string w_string_t;

typedef enum {
  W_STRING_BYTE,
  W_STRING_UNICODE,
  W_STRING_MIXED
} w_string_type_t;

struct watchman_string {
  std::atomic<long> refcnt;
  uint32_t _hval;
  uint32_t len;
  w_string_t *slice;
  const char *buf;
  w_string_type_t type:3;
  unsigned hval_computed:1;

  watchman_string();
  ~watchman_string();
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
bool w_string_contains_cstr_len(
    const w_string_t* str,
    const char* needle,
    uint32_t nlen);

void w_string_delref(w_string_t *str);
w_string_t *w_string_dirname(w_string_t *str);
char *w_string_dup_buf(const w_string_t *str);
w_string_t *w_string_dup_lower(w_string_t *str);

bool w_string_equal(const w_string_t *a, const w_string_t *b);
bool w_string_equal_caseless(const w_string_t *a, const w_string_t *b);
bool w_string_equal_cstring(const w_string_t *a, const char *b);

void w_string_in_place_normalize_separators(w_string_t **str, char target_sep);

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

uint32_t w_string_embedded_size(w_string_t *str);
void w_string_embedded_copy(w_string_t *dest, w_string_t *src);

struct watchman_dir;
w_string_t *w_dir_copy_full_path(const struct watchman_dir *dir);
w_string_t* w_dir_path_cat_cstr_len(
    const struct watchman_dir* dir,
    const char* extra,
    uint32_t extra_len);
w_string_t* w_dir_path_cat_cstr(
    const struct watchman_dir* dir,
    const char* extra);
w_string_t* w_dir_path_cat_str(const struct watchman_dir* dir, w_string_t* str);

bool w_is_path_absolute_cstr(const char *path);
bool w_is_path_absolute_cstr_len(const char *path, uint32_t len);

static inline bool is_slash(char c) {
  return (c == '/') || (c == '\\');
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
      typename std::enable_if<std::is_class<String>::value>::type = 0>
  inline /* implicit */ w_string_piece(const String& str)
      : s_(str.data()), e_(str.data() + str.size()) {}

  inline /* implicit */ w_string_piece(const char* cstr)
      : s_(cstr), e_(cstr + strlen(cstr)) {}

  /* implicit */ inline w_string_piece(const char* cstr, size_t len)
      : s_(cstr), e_(cstr + len) {}

  w_string_piece(const w_string_piece& other) = default;
  w_string_piece(w_string_piece&& other) noexcept;

  inline const char* data() const {
    return s_;
  }

  inline size_t size() const {
    return e_ - s_;
  }

  const char& operator[](size_t i) const {
    return s_[i];
  }

  /** Return a copy of the string as a w_string */
  w_string asWString(w_string_type_t stringType = W_STRING_BYTE) const;

  bool pathIsAbsolute() const;
  w_string_piece dirName() const;
  w_string_piece baseName() const;

  bool operator==(w_string_piece other) const;

  bool startsWith(w_string_piece prefix) const;
  bool startsWithCaseInsensitive(w_string_piece prefix) const;
};

/** A smart pointer class for tracking w_string_t instances */
class w_string {
 public:
  /** Initialize a nullptr */
  w_string();
  /* implicit */ w_string(std::nullptr_t);

  /** Make a new string from some bytes and a type */
  w_string(
      const char* buf,
      uint32_t len,
      w_string_type_t stringType = W_STRING_BYTE);
  /* implicit */ w_string(
      const char* buf,
      w_string_type_t stringType = W_STRING_BYTE);

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
  w_string_t *release();

  operator w_string_t*() const {
    return str_;
  }

  operator w_string_piece() const {
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
  static w_string printf(WATCHMAN_FMT_STRING(const char* format), ...)
      WATCHMAN_FMT_ATTR(1, 2);

  template <typename... Args>
  static w_string build(Args&&... args);

  /** Return a possibly new version of this string that is null terminated */
  w_string asNullTerminated() const;

  /** Ensure that this instance is referencing a null terminated version
   * of the current string */
  void makeNullTerminated();

  /** Returns a pointer to a null terminated c-string.
   * If this instance doesn't point to a null terminated c-string, throws
   * an exception that tells you to use asNullTerminated or makeNullTerminated
   * first. */
  const char* c_str() const;
  const char* data() const {
    return str_->buf;
  }
  size_t size() const {
    return str_->len;
  }

  w_string_type_t type() const {
    return str_->type;
  }

  /** Returns the directory component of the string, assuming a path string */
  w_string dirName() const;
  /** Returns the file name component of the string, assuming a path string */
  w_string baseName() const;
  /** Returns the filename suffix of a path string */
  w_string suffix() const;

  /** Returns a slice of this string */
  w_string slice(uint32_t start, uint32_t len) const;

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
}

/** helper for automatically releasing malloc'd memory.
 *
 * auto s = autofree(strdup("something"));
 * printf("%s\n", s.get());
 *
 * The implementation returns a unique_ptr that will free()
 * the memory when it falls out of scope. */
template <typename T>
std::unique_ptr<T, decltype(free) *> autofree(T* mem) {
  return std::unique_ptr<T, decltype(free)*>{mem, free};
}

namespace detail {

// Testing whether something is a known stringy type
// with .data(), .size() methods
template <typename T>
struct IsStringSrc {
  enum {
    value = std::is_same<T, std::string>::value ||
        std::is_same<T, w_string>::value ||
        std::is_same<T, w_string_piece>::value
  };
};

// w_string_t is immutable so it doesn't have an .append() method, so
// we use this helper class to keep track of where we're emitting the
// concatenated strings.
struct Appender {
  char* buf;
  char* limit;

  Appender(char* buf, size_t len) : buf(buf), limit(buf + len) {}

  // After placing data in the buffer, move the cursor forwards
  void advance(size_t amount) {
    w_assert(amount <= avail(), "advanced more than reserved space");
    buf += amount;
  }

  // How many bytes of space are available
  size_t avail() const {
    return limit - buf;
  }

  // Copy a buffer in and advance the cursor
  void append(const char* src, size_t amount) {
    w_assert(amount <= avail(), "advancing more than reserved space");
    memcpy(buf, src, amount);
    advance(amount);
  }

  // Helper for rendering a base10 number
  void appendUint64(uint64_t v) {
    // 20 is max possible decimal digits for a 64-bit number
    char localBuf[20];
    uint32_t pos = sizeof(localBuf) - 1;
    while (v >= 10) {
      auto const q = v / 10;
      auto const r = static_cast<uint32_t>(v % 10);
      localBuf[pos--] = '0' + r;
      v = q;
    }
    localBuf[pos] = static_cast<uint32_t>(v) + '0';
    append(localBuf + pos, sizeof(localBuf) - pos);
  }

  // Helper for rendering a base16 number
  void appendHexUint64(uint64_t v) {
    // 16 is max possible hex digits for a 64-bit number
    char localBuf[16];
    static const char* hexDigit = "0123456789abcdef";

    uint32_t pos = sizeof(localBuf) - 1;
    while (v >= 16) {
      auto const q = v / 16;
      auto const r = static_cast<uint32_t>(v % 16);
      localBuf[pos--] = hexDigit[r];
      v = q;
    }
    localBuf[pos] = hexDigit[static_cast<uint32_t>(v)];
    append(localBuf + pos, sizeof(localBuf) - pos);
  }
};

template <typename T>
constexpr typename std::enable_if<
    std::is_pointer<T>::value && !std::is_convertible<T, const char*>::value,
    size_t>::type
estimateSpaceNeeded(T) {
  // We render pointers as 0xXXXXXX, so we need a 2 byte prefix
  // and then an appropriate number of hex digits for the pointer size.
  return 2 + (sizeof(T) * 2);
}

// Render pointers as lowercase hex
template <typename T>
typename std::enable_if<
    std::is_pointer<T>::value &&
    !std::is_convertible<T, const char*>::value>::type
toAppend(T value, Appender& result) {
  result.append("0x", 2);
  result.appendHexUint64(uintptr_t(value));
}

// Defer to snprintf for rendering floating point values
inline size_t estimateSpaceNeeded(double val) {
  return snprintf(nullptr, 0, "%f", val);
}

inline void toAppend(double val, Appender& result) {
  auto len = snprintf(result.buf, result.avail(), "%f", val);
  result.advance(len);
}

// Need to specialize nullptr otherwise we see it as an empty w_string_piece
constexpr size_t estimateSpaceNeeded(std::nullptr_t) {
  return 3; // 0x0
}

inline void toAppend(std::nullptr_t, Appender& result) {
  result.append("0x0", 3);
}

// Size an unsigned integer value
template <typename T>
constexpr typename std::enable_if<
    std::is_integral<T>::value && !std::is_signed<T>::value,
    size_t>::type
estimateSpaceNeeded(T) {
  // approx 0.3 decimal digits per binary bit; round up.
  // 8 bits -> 3 digits
  // 16 bits -> 5 digits
  // 32 bits -> 10 digits
  // 64 bits -> 20 digits
  // Note that for larger bit sizes it is more memory efficient
  // to compute the size on a per value basis (eg: if a 64-bit value
  // fits in a single decimal digit, we're 20x more wasteful with
  // memory).  We're going for relative simplicity here; just
  // one simple function to measure up and ensure we have enough
  // room.
  return 1 + (sizeof(T) * 8 * 0.3);
}

// Render an unsigned integer value
template <typename T>
typename std::enable_if<
    std::is_integral<T>::value && !std::is_signed<T>::value>::type
toAppend(T value, Appender& result) {
  result.appendUint64(value);
}

template <typename T>
constexpr typename std::enable_if<
    std::is_integral<T>::value && std::is_signed<T>::value,
    size_t>::type
estimateSpaceNeeded(T) {
  // Need 1 extra byte for the '-' sign
  return 2 + (sizeof(T) * 8 * 0.3);
}

// Render a signed integer value
template <typename T>
typename std::enable_if<
    std::is_integral<T>::value && std::is_signed<T>::value>::type
toAppend(T value, Appender& result) {
  if (value < 0) {
    result.append("-", 1);
    // When "value" is the smallest negative, negating it would
    // evoke undefined behavior, so instead of "-value" we carry
    // out a bitwise complement and add one.
    result.appendUint64(~static_cast<uint64_t>(value) + 1);
  } else {
    result.appendUint64(static_cast<uint64_t>(value));
  }
}

// Measure a stringy value
template <typename T>
typename std::enable_if<IsStringSrc<T>::value, size_t>::type
estimateSpaceNeeded(const T& src) {
  // For known stringy types, we call the size method
  return src.size();
}

template <typename T>
typename std::enable_if<IsStringSrc<T>::value>::type toAppend(
    const T& src,
    Appender& result) {
  result.append(src.data(), src.size());
}

// If we can convert it to a string piece, we can call its size() method
template <typename T>
typename std::enable_if<std::is_convertible<T, const char*>::value, size_t>::
    type
    estimateSpaceNeeded(T src) {
  return w_string_piece(src).size();
}

template <typename T>
typename std::enable_if<std::is_convertible<T, const char*>::value>::type
toAppend(T src, Appender& result) {
  w_string_piece piece(src);
  result.append(piece.data(), piece.size());
}

// If we can convert it to a string piece, we can call its size() method
template <typename T>
typename std::enable_if<
    std::is_convertible<T, w_string_piece>::value && !IsStringSrc<T>::value &&
        !std::is_convertible<T, const char*>::value,
    size_t>::type
estimateSpaceNeeded(T src) {
  return w_string_piece(src).size();
}

template <typename T>
typename std::enable_if<
    std::is_convertible<T, w_string_piece>::value && !IsStringSrc<T>::value &&
    !std::is_convertible<T, const char*>::value>::type
toAppend(T src, Appender& result) {
  w_string_piece piece(src);
  result.append(piece.data(), piece.size());
}

// Recursive, variadic expansion to measure up a set of arguments

inline size_t estimateSpaceToReserve(size_t accumulated) {
  return accumulated;
}

template <typename T, typename... Args>
size_t
estimateSpaceToReserve(size_t accumulated, const T& v, const Args&... args) {
  return estimateSpaceToReserve(accumulated + estimateSpaceNeeded(v), args...);
}

// Recursive, variadic expansion to append a set of arguments

inline void appendTo(Appender&) {}

template <typename T, typename... Args>
void appendTo(Appender& result, const T& v, const Args&... args) {
  toAppend(v, result);
  appendTo(result, args...);
}
} // namespace detail

// Concat the args together into a w_string

template <typename... Args>
w_string w_string::build(Args&&... args) {
  auto reserved = detail::estimateSpaceToReserve(1, args...);

  auto s = (w_string_t*)(new char[sizeof(w_string_t) + reserved]);
  new (s) watchman_string();

  s->refcnt = 1;
  auto buf = (char*)(s + 1);
  s->buf = buf;

  detail::Appender appender(buf, reserved);
  detail::appendTo(appender, args...);

  s->len = reserved - appender.avail();
  buf[s->len] = '\0';

  return w_string(s, false);
}

#endif

/* vim:ts=2:sw=2:et:
 */
