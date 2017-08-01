/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Note that this is a port of folly/ScopeGuard.h, adjusted to compile
// in watchman without pulling in all of folly (a non-goal for the watchman
// project at the current time).
// It defines SCOPE_XXX symbols that will conflict with folly/ScopeGuard.h
// if both are in use.  Therefore the convention is that only watchman .cpp
// files will include this.  For the small handful of watchman files that
// pull in folly dependencies, those files will prefer to include
// folly/ScopeGuard.h instead.  In the longer run (once gcc 5 is more easily
// installable for our supported linux systems) and once the homebrew build
// story for our various projects is in better shape, we can remove this in
// favor of just depending on folly.

#pragma once
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>
#include "watchman_preprocessor.h"

#if defined(__GNUG__) || defined(__clang__)
#define WATCHMAN_EXCEPTION_COUNT_USE_CXA_GET_GLOBALS
namespace __cxxabiv1 {
// forward declaration (originally defined in unwind-cxx.h from from libstdc++)
struct __cxa_eh_globals;
// declared in cxxabi.h from libstdc++-v3
extern "C" __cxa_eh_globals* __cxa_get_globals() noexcept;
}
#elif defined(_MSC_VER) && (_MSC_VER >= 1400) && \
    (_MSC_VER < 1900) // MSVC++ 8.0 or greater
#define WATCHMAN_EXCEPTION_COUNT_USE_GETPTD
// forward declaration (originally defined in mtdll.h from MSVCRT)
struct _tiddata;
extern "C" _tiddata* _getptd(); // declared in mtdll.h from MSVCRT
#elif defined(_MSC_VER) && (_MSC_VER >= 1900) // MSVC++ 2015
#define WATCHMAN_EXCEPTION_COUNT_USE_STD
#else
// Raise an error when trying to use this on unsupported platforms.
#error "Unsupported platform, don't include this header."
#endif

namespace watchman {
namespace detail {

/**
 * Used to check if a new uncaught exception was thrown by monitoring the
 * number of uncaught exceptions.
 *
 * Usage:
 *  - create a new UncaughtExceptionCounter object
 *  - call isNewUncaughtException() on the new object to check if a new
 *    uncaught exception was thrown since the object was created
 */
class UncaughtExceptionCounter {
 public:
  UncaughtExceptionCounter() noexcept
      : exceptionCount_(getUncaughtExceptionCount()) {}

  UncaughtExceptionCounter(const UncaughtExceptionCounter& other) noexcept
      : exceptionCount_(other.exceptionCount_) {}

  bool isNewUncaughtException() noexcept {
    return getUncaughtExceptionCount() > exceptionCount_;
  }

 private:
  int getUncaughtExceptionCount() noexcept;

  int exceptionCount_;
};

/**
 * Returns the number of uncaught exceptions.
 *
 * This function is based on Evgeny Panasyuk's implementation from here:
 * http://fburl.com/15190026
 */
inline int UncaughtExceptionCounter::getUncaughtExceptionCount() noexcept {
#if defined(WATCHMAN_EXCEPTION_COUNT_USE_CXA_GET_GLOBALS)
  // __cxa_get_globals returns a __cxa_eh_globals* (defined in unwind-cxx.h).
  // The offset below returns __cxa_eh_globals::uncaughtExceptions.
  return *(reinterpret_cast<unsigned int*>(
      static_cast<char*>(static_cast<void*>(__cxxabiv1::__cxa_get_globals())) +
      sizeof(void*)));
#elif defined(WATCHMAN_EXCEPTION_COUNT_USE_GETPTD)
  // _getptd() returns a _tiddata* (defined in mtdll.h).
  // The offset below returns _tiddata::_ProcessingThrow.
  return *(reinterpret_cast<int*>(
      static_cast<char*>(static_cast<void*>(_getptd())) + sizeof(void*) * 28 +
      0x4 * 8));
#elif defined(WATCHMAN_EXCEPTION_COUNT_USE_STD)
  return std::uncaught_exceptions();
#endif
}

} // namespace detail

/**
 * ScopeGuard is a general implementation of the "Initialization is
 * Resource Acquisition" idiom.  Basically, it guarantees that a function
 * is executed upon leaving the currrent scope unless otherwise told.
 *
 * The makeGuard() function is used to create a new ScopeGuard object.
 * It can be instantiated with a lambda function, a std::function<void()>,
 * a functor, or a void(*)() function pointer.
 *
 *
 * Usage example: Add a friend to memory if and only if it is also added
 * to the db.
 *
 * void User::addFriend(User& newFriend) {
 *   // add the friend to memory
 *   friends_.push_back(&newFriend);
 *
 *   // If the db insertion that follows fails, we should
 *   // remove it from memory.
 *   // (You could also declare this as "auto guard = makeGuard(...)")
 *   ScopeGuard guard = makeGuard([&] { friends_.pop_back(); });
 *
 *   // this will throw an exception upon error, which
 *   // makes the ScopeGuard execute UserCont::pop_back()
 *   // once the Guard's destructor is called.
 *   db_->addFriend(GetName(), newFriend.GetName());
 *
 *   // an exception was not thrown, so don't execute
 *   // the Guard.
 *   guard.dismiss();
 * }
 *
 * Examine ScopeGuardTest.cpp for some more sample usage.
 *
 * Stolen from:
 *   Andrei's and Petru Marginean's CUJ article:
 *     http://drdobbs.com/184403758
 *   and the loki library:
 *     http://loki-lib.sourceforge.net/index.php?n=Idioms.ScopeGuardPointer
 *   and triendl.kj article:
 *     http://www.codeproject.com/KB/cpp/scope_guard.aspx
 */
class ScopeGuardImplBase {
 public:
  void dismiss() noexcept {
    dismissed_ = true;
  }

 protected:
  ScopeGuardImplBase() noexcept : dismissed_(false) {}

  static ScopeGuardImplBase makeEmptyScopeGuard() noexcept {
    return ScopeGuardImplBase{};
  }

  template <typename T>
  static const T& asConst(const T& t) noexcept {
    return t;
  }

  bool dismissed_;
};

template <typename FunctionType>
class ScopeGuardImpl : public ScopeGuardImplBase {
 public:
  explicit ScopeGuardImpl(FunctionType& fn) noexcept(
      std::is_nothrow_copy_constructible<FunctionType>::value)
      : ScopeGuardImpl(
            asConst(fn),
            makeFailsafe(
                std::is_nothrow_copy_constructible<FunctionType>{},
                &fn)) {}

  explicit ScopeGuardImpl(const FunctionType& fn) noexcept(
      std::is_nothrow_copy_constructible<FunctionType>::value)
      : ScopeGuardImpl(
            fn,
            makeFailsafe(
                std::is_nothrow_copy_constructible<FunctionType>{},
                &fn)) {}

  explicit ScopeGuardImpl(FunctionType&& fn) noexcept(
      std::is_nothrow_move_constructible<FunctionType>::value)
      : ScopeGuardImpl(
            std::move_if_noexcept(fn),
            makeFailsafe(
                std::is_nothrow_move_constructible<FunctionType>{},
                &fn)) {}

  ScopeGuardImpl(ScopeGuardImpl&& other) noexcept(
      std::is_nothrow_move_constructible<FunctionType>::value)
      : function_(std::move_if_noexcept(other.function_)) {
    // If the above line attempts a copy and the copy throws, other is
    // left owning the cleanup action and will execute it (or not) depending
    // on the value of other.dismissed_. The following lines only execute
    // if the move/copy succeeded, in which case *this assumes ownership of
    // the cleanup action and dismisses other.
    dismissed_ = other.dismissed_;
    other.dismissed_ = true;
  }

  ~ScopeGuardImpl() noexcept {
    if (!dismissed_) {
      execute();
    }
  }

 private:
  static ScopeGuardImplBase makeFailsafe(std::true_type, const void*) noexcept {
    return makeEmptyScopeGuard();
  }

  template <typename Fn>
  static auto makeFailsafe(std::false_type, Fn* fn) noexcept
      -> ScopeGuardImpl<decltype(std::ref(*fn))> {
    return ScopeGuardImpl<decltype(std::ref(*fn))>{std::ref(*fn)};
  }

  template <typename Fn>
  explicit ScopeGuardImpl(Fn&& fn, ScopeGuardImplBase&& failsafe)
      : ScopeGuardImplBase{}, function_(std::forward<Fn>(fn)) {
    failsafe.dismiss();
  }

  void* operator new(std::size_t) = delete;

  void execute() noexcept {
    function_();
  }

  FunctionType function_;
};

template <typename FunctionType>
ScopeGuardImpl<typename std::decay<FunctionType>::type>
makeGuard(FunctionType&& fn) noexcept(std::is_nothrow_constructible<
                                      typename std::decay<FunctionType>::type,
                                      FunctionType>::value) {
  return ScopeGuardImpl<typename std::decay<FunctionType>::type>(
      std::forward<FunctionType>(fn));
}

/**
 * This is largely unneeded if you just use auto for your guards.
 */
typedef ScopeGuardImplBase&& ScopeGuard;

namespace detail {

#if defined(WATCHMAN_EXCEPTION_COUNT_USE_CXA_GET_GLOBALS) || \
    defined(WATCHMAN_EXCEPTION_COUNT_USE_GETPTD) ||          \
    defined(WATCHMAN_EXCEPTION_COUNT_USE_STD)

/**
 * ScopeGuard used for executing a function when leaving the current scope
 * depending on the presence of a new uncaught exception.
 *
 * If the executeOnException template parameter is true, the function is
 * executed if a new uncaught exception is present at the end of the scope.
 * If the parameter is false, then the function is executed if no new uncaught
 * exceptions are present at the end of the scope.
 *
 * Used to implement SCOPE_FAIL and SCOPE_SUCCES below.
 */
template <typename FunctionType, bool executeOnException>
class ScopeGuardForNewException {
 public:
  explicit ScopeGuardForNewException(const FunctionType& fn) : function_(fn) {}

  explicit ScopeGuardForNewException(FunctionType&& fn)
      : function_(std::move(fn)) {}

  ScopeGuardForNewException(ScopeGuardForNewException&& other)
      : function_(std::move(other.function_)),
        exceptionCounter_(std::move(other.exceptionCounter_)) {}

  ~ScopeGuardForNewException() noexcept(executeOnException) {
    if (executeOnException == exceptionCounter_.isNewUncaughtException()) {
      function_();
    }
  }

 private:
  ScopeGuardForNewException(const ScopeGuardForNewException& other) = delete;

  void* operator new(std::size_t) = delete;

  FunctionType function_;
  UncaughtExceptionCounter exceptionCounter_;
};

/**
 * Internal use for the macro SCOPE_FAIL below
 */
enum class ScopeGuardOnFail {};

template <typename FunctionType>
ScopeGuardForNewException<typename std::decay<FunctionType>::type, true>
operator+(detail::ScopeGuardOnFail, FunctionType&& fn) {
  return ScopeGuardForNewException<
      typename std::decay<FunctionType>::type,
      true>(std::forward<FunctionType>(fn));
}

/**
 * Internal use for the macro SCOPE_SUCCESS below
 */
enum class ScopeGuardOnSuccess {};

template <typename FunctionType>
ScopeGuardForNewException<typename std::decay<FunctionType>::type, false>
operator+(ScopeGuardOnSuccess, FunctionType&& fn) {
  return ScopeGuardForNewException<
      typename std::decay<FunctionType>::type,
      false>(std::forward<FunctionType>(fn));
}

#endif // native uncaught_exception() supported

/**
 * Internal use for the macro SCOPE_EXIT below
 */
enum class ScopeGuardOnExit {};

template <typename FunctionType>
ScopeGuardImpl<typename std::decay<FunctionType>::type> operator+(
    detail::ScopeGuardOnExit,
    FunctionType&& fn) {
  return ScopeGuardImpl<typename std::decay<FunctionType>::type>(
      std::forward<FunctionType>(fn));
}
} // namespace detail

#define SCOPE_EXIT                      \
  auto w_gen_symbol(SCOPE_EXIT_STATE) = \
      ::watchman::detail::ScopeGuardOnExit() + [&]() noexcept

#if defined(WATCHMAN_EXCEPTION_COUNT_USE_CXA_GET_GLOBALS) || \
    defined(WATCHMAN_EXCEPTION_COUNT_USE_GETPTD) ||          \
    defined(WATCHMAN_EXCEPTION_COUNT_USE_STD)
#define SCOPE_FAIL                      \
  auto w_gen_symbol(SCOPE_FAIL_STATE) = \
      ::watchman::detail::ScopeGuardOnFail() + [&]() noexcept

#define SCOPE_SUCCESS                      \
  auto w_gen_symbol(SCOPE_SUCCESS_STATE) = \
      ::watchman::detail::ScopeGuardOnSuccess() + [&]()
#endif // native uncaught_exception() supported

} // namespace watchman
