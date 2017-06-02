/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <string>
#include <system_error>

// We may have to deal with errors from various sources and with
// different error namespaces.  This header defines some helpers
// to make it easier for code to reason about and react to those errors.
// http://blog.think-async.com/2010/04/system-error-support-in-c0x-part-5.html
// explains the concepts behind the error_condition API used here.

namespace watchman {

// Various classes of errors that we wish to programmatially respond to.
// This doesn't need to be an exhaustive list of all possible conditions,
// just those that we want to handle in code.
enum class error_code {
  no_such_file_or_directory,
  not_a_directory,
  too_many_symbolic_link_levels,
  permission_denied,
  system_limits_exceeded,
  timed_out,
  not_a_symlink,
};

// An error category implementation that is present for comparison purposes
// only; do not use this category to create error_codes explicitly.
class error_category : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int err) const override;
  bool equivalent(const std::error_code& code, int condition) const
      noexcept override;
};

// Obtain a ref to the above error category
const std::error_category& error_category();

// Helper that is used to implicitly construct an error condition
// during equivalence testing.  Don't use this directly.
inline std::error_condition make_error_condition(error_code e) {
  return std::error_condition(static_cast<int>(e), error_category());
}

// Helper that is used to implicitly construct an error code
// during equivalence testing.  Don't use this directly.
inline std::error_code make_error_code(error_code e) {
  return std::error_code(static_cast<int>(e), error_category());
}

// A type representing windows error codes.  These are stored
// in DWORD values and it is not really feasible to enumerate all
// possible values due to the way that windows error codes are
// structured.
// We use uint32_t here to avoid pulling in the windows header file
// here implicitly.
// We use this purely for convenience with the make_error_XXX
// functions below.  While this is only used on Windows, it doesn't
// hurt to define it unconditionally for all platforms here.
enum windows_error_code : uint32_t {};

// Helper that is used to implicitly construct an windows error condition
// during equivalence testing.  This only makes sense to use on
// windows platforms.
inline std::error_condition make_error_condition(windows_error_code e) {
  return std::error_condition(static_cast<int>(e), std::system_category());
}

// Helper that is used to implicitly construct an windows error code
// during equivalence testing.  This only makes sense to use on
// windows platforms.
inline std::error_code make_error_code(windows_error_code e) {
  return std::error_code(static_cast<int>(e), std::system_category());
}

// An error category for explaining inotify specific errors.
// It is effectively the same as generic_category except that
// the messages for some of the codes are different.
class inotify_category : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int err) const override;
};

// Obtain a ref to the above error category
const std::error_category& inotify_category();

} // namespace watchman

// Allow watchman::error_code to implicitly convert to std::error_condition
namespace std {
template <>
struct is_error_condition_enum<watchman::error_code> : public true_type {};

// Allow watchman::windows_error_code to implicitly convert to
// std::error_condition
template <>
struct is_error_condition_enum<watchman::windows_error_code>
    : public true_type {};
}
