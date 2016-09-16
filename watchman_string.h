/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#ifndef WATCHMAN_STRING_H
#define WATCHMAN_STRING_H

#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
#include <memory>
#endif

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

#ifdef __cplusplus
  // our jansson fork depends on the C API, so hide this c++ism from it
  watchman_string();
  ~watchman_string();
#endif
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

#ifdef __cplusplus
class w_string;

/** Represents a view over some externally managed string storage.
 * It is simply a pair of pointers that define the start and end
 * of the valid region. */
class w_string_piece {
  const char *s_, *e_;

 public:
  w_string_piece();
  w_string_piece(std::nullptr_t);

  inline w_string_piece(w_string_t* str)
      : s_(str->buf), e_(str->buf + str->len) {}

  /** Construct from a string-like object */
  template <typename String,
            typename std::enable_if<std::is_class<String>::value>::type = 0>
  inline w_string_piece(const String &str)
      : s_(str.data()), e_(str.data() + str.size()) {}

  inline w_string_piece(const char* cstr) : s_(cstr), e_(cstr + strlen(cstr)){};
  inline w_string_piece(const char* cstr, size_t len)
      : s_(cstr), e_(cstr + len){};

  w_string_piece(const w_string_piece& other) = default;
  w_string_piece(w_string_piece&& other);

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
  w_string(std::nullptr_t);

  /** Make a new string from some bytes and a type */
  w_string(
      const char* buf,
      uint32_t len,
      w_string_type_t stringType = W_STRING_BYTE);
  w_string(
      const char* buf,
      w_string_type_t stringType = W_STRING_BYTE);

  /** Initialize, taking a ref on w_string_t */
  w_string(w_string_t* str, bool addRef = true);
  /** Release the string reference when we fall out of scope */
  ~w_string();
  /** Copying adds a ref */
  w_string(const w_string& other);
  w_string& operator=(const w_string& other);
  /** Moving steals a ref */
  w_string(w_string&& other);
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

  /** path concatenation
   * Pass in a list of w_string_pieces to join them all similarly to
   * the python os.path.join() function. */
  static w_string pathCat(std::initializer_list<w_string_piece> elems);

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

  /** Returns the directory component of the string, assuming a path string */
  w_string dirName() const;
  /** Returns the file name component of the string, assuming a path string */
  w_string baseName() const;
  /** Returns the filename suffix of a path string */
  w_string suffix() const;

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

#endif

#endif

/* vim:ts=2:sw=2:et:
 */
