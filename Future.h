/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include "Result.h"

namespace watchman {

// An executor is a very light abstraction over something that can run
// some function.  We use this to perform the WaitableResult callback
// in some other context (eg: ThreadPool).  In practice, this is used
// to move the execution context for a .then call to a worker thread.
struct Executor {
  virtual ~Executor() = default;
  virtual void run(std::function<void()>&& func) = 0;
};

// WaitableResult<T> encapsulates a Result<T> and allows waiting and notifying
// and interested party.  You are not expected to create an instance of this
// class directly; it is used as the shared state between the Promise and
// Future classes.
template <typename T>
class WaitableResult : public std::enable_shared_from_this<WaitableResult<T>> {
  static_assert(!std::is_same<T, void>::value, "use Unit instead of void");

 public:
  WaitableResult() = default;
  explicit WaitableResult(Result<T>&& t) : result_(std::move(t)) {}
  WaitableResult(const WaitableResult&) = delete;
  WaitableResult(WaitableResult&&) = delete;

  // Assign to the underlying Result<T>.
  // After assigning, dispatch any associated callback and notify any waiters
  template <typename U>
  void assign(U&& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    result_ = Result<T>(std::forward<U>(value));
    maybeCallback(std::move(lock));
  }

  // Get a reference to the enclosed result
  Result<T>& result() {
    return result_;
  }

  // Wait until the result is no longer empty
  void wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return !result_.empty(); });
  }

  // Wait for up to the specified duration.
  // Returns true as soon as the result is no longer empty.
  // Returns false if the result is still empty after duration has
  // passed.
  template <class Rep, class Period>
  bool wait_for(const std::chrono::duration<Rep, Period>& duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, duration, [&] { return !result_.empty(); });
  }

  // Associate a callback with the result.
  // This is intended for use by internal plumbing and a casual
  // user of a Future should not call this method; instead,
  // you want to use Future::then().
  //
  // The callback will be dispatched when the assign() method
  // is called.  If the assign() method was called prior to
  // setCallback(), it will be called by setCallback().
  //
  // It is possible for the callback to fire twice for the
  // same Future if setExecutor() was used to assign an
  // executor that runs immediately, and if the callback
  // throws an exception. If this happens, callback will be
  // called a second time with the Result containing the
  // exception that it called previously.  Throwing an
  // exception in that context will bubble up to the caller
  // of assign().
  //
  // If you stick to Future::then(), you don't need to
  // worry about this.
  template <typename Func>
  void setCallback(Func&& func) {
    std::unique_lock<std::mutex> lock(mutex_);
    callback_ = std::forward<Func>(func);
    if (!result_.empty()) {
      maybeCallback(std::move(lock));
    }
  }

  // Change the executor associated with the future
  void setExecutor(Executor* executor) {
    std::unique_lock<std::mutex> lock(mutex_);
    executor_ = executor;
  }

 private:
  Result<T> result_;
  mutable std::condition_variable condition_;
  mutable std::mutex mutex_;
  std::function<void(Result<T>&&)> callback_;
  Executor* executor_{nullptr};

  // If a callback is set, call it.
  // Then notify any waiters that the result is available.
  void maybeCallback(std::unique_lock<std::mutex>&& lock) {
    if (result_.empty()) {
      return;
    }

    if (callback_) {
      // Ensure that we are kept alive while we dispatch the callback
      auto scope_guard = this->shared_from_this();

      // Steal the callback
      std::function<void(Result<T> &&)> func;
      std::swap(func, callback_);

      // For safety, ensure that we are unlocked while calling the callback.
      // While the intent is that the executor run the callback in a
      // different thread context, it may choose to run something
      // immediately.
      lock.unlock();

      if (executor_) {
        try {
          // Unfortunately, have to make a copy of func here in
          // order to have sane exception handling
          executor_->run([scope_guard, this, func] {
            func(std::move(result_));
          });
          condition_.notify_all();
          return;
        } catch (const std::exception& exc) {
          // We get here if executor_->run() threw an exception.
          // This is most likely to happen if the thread pool is
          // full, but we can't make any assumptions of the nature
          // of the exception; it may have simply run the callback
          // immediately, and we're just seeing the exception from
          // the callback here.
          // We're really only capturing this so that we can
          // propagate thread pool errors through the exception
          // chain.

          // Replace the current value with the exception, and we'll
          // dispatch the exception to the callback below.
          result_ = Result<T>(std::current_exception());
        }
      }

      // We don't catch and propagate exceptions that this function
      // throws (like we do in the executor case above), because
      // the callback is supposed to manage exceptions and do
      // the right thing for itself.
      func(std::move(result_));
    }
    condition_.notify_all();
  }
};

template <typename T>
class Promise;

template <typename T>
class Future;

// A little bit of helper glue for unwrapping Future<Future<T>>
template <typename T>
struct isFuture : std::false_type {
  using Inner = typename Unit::Lift<T>::type;
};

template <typename T>
struct isFuture<Future<T>> : std::true_type {
  using Inner = T;
};

// Extracts the return type of a functor call
template <typename F, typename... Args>
using resultOf = decltype(std::declval<F>()(std::declval<Args>()...));

// The Future is the client side of the Promise/Future pair.
// A Promise can return a single instance of a Future.  The two
// are linked by a shared WaitableResult object.
// A Future is only safe for access from a single thread at a time.
template <typename T>
class Future {
  static_assert(!std::is_same<T, void>::value, "use Unit instead of void");

 public:
  using value_type = T;

  Future() noexcept = default;
  // Moveable
  Future(Future&&) noexcept = default;
  Future& operator=(Future&&) noexcept = default;
  // Not copyable
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;
  // makeFuture() uses this to build ready Future instances
  explicit Future(std::shared_ptr<WaitableResult<T>> state) : state_(state) {}

  // Block until the associated Promise is fulfilled
  void wait() const {
    if (!state_) {
      throw std::logic_error("Future has no shared state");
    }
    state_->wait();
  }

  // Wait for up to the specified duration for the associated Promise to
  // be fulfilled.
  // Returns true as soon as the Promise is fulfilled.
  // Returns false if the Promise was not fulfilled within the specified
  // duration.
  template <class Rep, class Period>
  bool wait_for(const std::chrono::duration<Rep, Period>& duration) const {
    if (!state_) {
      throw std::logic_error("Future has no shared state");
    }
    return state_->wait_for(duration);
  }

  // Returns true if the associated Promise has been fulfilled.
  bool isReady() const {
    return this->wait_for(std::chrono::milliseconds(0));
  }

  // Waits for the Promise to be fulfilled, then returns a reference
  // to the value in the promise.
  // If the Result holds an error this will cause the error to be
  // thrown.
  T& get() {
    return result().value();
  }

  // As get() above, but returns a const reference.
  const T& get() const {
    return result().value();
  }

  // Waits for the Promise to be fulfilled, then returns a reference
  // to the Result in the promise.
  Result<T>& result() {
    wait();
    return state_->result();
  }

  // Waits for the Promise to be fulfilled, then returns a const reference
  // to the Result in the promise.
  const Result<T>& result() const {
    wait();
    return state_->result();
  }

  // Chain a future together with some action to happen once
  // it is ready.
  // f.then([](Result<T>&& result) { return something; })
  // This handles the case where something is not a Future<>
  template <typename Func>
  typename std::enable_if<
      !isFuture<resultOf<Func, Result<T>&&>>::value,
      Future<typename isFuture<resultOf<Func, Result<T>&&>>::Inner>>::type
  then(Func&& func) {
    using Ret = typename isFuture<resultOf<Func, Result<T>&&>>::Inner;
    struct thenState {
      Promise<Ret> promise;
      Func func;
      thenState(Func&& func) : func(std::forward<Func>(func)) {}
    };
    auto state = std::make_shared<thenState>(std::forward<Func>(func));
    auto result = state->promise.getFuture();

    state_->setCallback([state](Result<T>&& result) {
      state->promise.setResult(makeResultWith(
          [&]() mutable { return state->func(std::move(result)); }));
    });

    return result;
  }

  // Chain a future together with some action to happen once
  // it is ready.
  // f.then([](Result<T>&& result) { return something; })
  // This handles the case where something is a Future<> and
  // unwraps it so that the result of .then is Future<> rather
  // than Future<Future<>>.
  template <typename Func>
  typename std::enable_if<
      isFuture<resultOf<Func, Result<T>&&>>::value,
      Future<typename isFuture<resultOf<Func, Result<T>&&>>::Inner>>::type
  then(Func&& func) {
    using Ret = resultOf<Func, Result<T>&&>;
    using InnerRet = typename isFuture<Ret>::Inner;

    struct thenState {
      Promise<InnerRet> promise;
      Func func;
      thenState(Func&& func) : func(std::forward<Func>(func)) {}
    };
    auto state = std::make_shared<thenState>(std::forward<Func>(func));
    auto result = state->promise.getFuture();

    state_->setCallback([state](Result<T>&& res) {
      try {
        auto future = state->func(std::move(res));
        future.setCallback([state](Result<InnerRet>&& res2) {
          state->promise.setResult(std::move(res2));
        });
      } catch (const std::exception& exc) {
        state->promise.setException(std::current_exception());
      }
    });

    return result;
  }

  // Exposes setCallback for .then when unwrapping Future<Future<>>.
  // Since Future<Future<T>> is a different class from Future<T>,
  // we have to make this public.
  // You probably want to use .then() and not this directly.
  template <typename Func>
  void setCallback(Func&& func) {
    state_->setCallback(std::forward<Func>(func));
  }

  // Returns a future with its execution context switched to the provided
  // executor.  A subsequent then() call will be dispatched by that
  // executor.  Note that, depending on timing, this sequence:
  // makeFuture().via(exec).then(A).then(B)
  // will execute A in the context of exec, but may execute B either in
  // exec or the current context.  If you need to ensure the execution
  // context for a then() call, you must precede it with a via() call
  // and supply the appropriate executor:
  // makeFuture().via(exec).then(A).via(exec).then(B)
  Future<T> via(Executor* executor) && {
    state_->setExecutor(executor);
    return std::move(*this);
  }

 private:
  std::shared_ptr<WaitableResult<T>> state_;
};

// The Promise is the server side of the Promise/Future pair.
template <typename T>
class Promise {
  static_assert(!std::is_same<T, void>::value, "use Unit instead of void");

 public:
  // Default construct to an un-fulfilled Promise
  Promise() : state_(std::make_shared<WaitableResult<T>>()) {}
  // Moveable
  Promise(Promise&&) noexcept = default;
  Promise& operator=(Promise&&) noexcept = default;
  // Copyable
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // Fulfills the promise with a value of type T.
  // Causes any waiters/callbacks associated with the Promise to be
  // notified/dispatched.
  // It is an error fulfill the same promise multiple times.
  void setValue(T&& value) {
    setResult(Result<T>(std::forward<T>(value)));
  }

  // Fulfills the promise with a Result<T>.
  // Causes any waiters/callbacks associated with the Promise to be
  // notified/dispatched.
  // It is an error fulfill the same promise multiple times.
  void setResult(Result<T>&& result) {
    if (assigned_) {
      throw std::logic_error("Promise already fulfilled");
    }
    assigned_ = true;
    state_->assign(std::move(result));
  }

  // Fulfills the promise with an exception.
  // Causes any waiters/callbacks associated with the Promise to be
  // notified/dispatched.
  // It is an error fulfill the same promise multiple times.
  void setException(std::exception_ptr exc) {
    setResult(Result<T>(exc));
  }

  // Returns a Future associated with this Promise.  The Future
  // allows a client to wait for the results.
  // It is an error to call getFuture() multiple times.
  Future<T> getFuture() {
    if (gotFuture_) {
      throw std::logic_error("Future already obtained");
    }
    gotFuture_ = true;
    return Future<T>(state_);
  }

 private:
  std::shared_ptr<WaitableResult<T>> state_;
  bool gotFuture_{false};
  bool assigned_{false};
};

// Convert a Result<T> to a Future<T> that is ready immediately
template <typename T>
Future<T> makeFuture(Result<T>&& t) {
  return Future<T>(std::make_shared<WaitableResult<T>>(std::move(t)));
}

// Helper for making an already fulfilled Future from a value;
// auto-deduces the Value type.
template <typename T>
Future<typename std::decay<T>::type> makeFuture(T&& t) {
  return makeFuture(Result<typename std::decay<T>::type>(std::forward<T>(t)));
}

inline Future<Unit> makeFuture() {
  return makeFuture(Result<Unit>(Unit{}));
}

// Yields a Future holding a vector<Result<T>> for each of the input futures
template <typename InputIterator>
Future<std::vector<Result<
    typename std::iterator_traits<InputIterator>::value_type::value_type>>>
collectAll(InputIterator first, InputIterator last) {
  using T =
      typename std::iterator_traits<InputIterator>::value_type::value_type;
  struct CollectAll {
    // Pre-allocate enough room for all results to avoid needing to
    // synchronize when the callback assigns everything
    CollectAll(size_t n) : results(n) {}

    // The destructor triggers the assignment and fulfillment of the Promise
    ~CollectAll() {
      p.setValue(std::move(results));
    }

    Promise<std::vector<Result<T>>> p;
    std::vector<Result<T>> results;
  };

  auto state = std::make_shared<CollectAll>(std::distance(first, last));
  size_t i = 0;
  while (first != last) {
    first->setCallback([state, i](Result<T>&& result) {
      state->results[i] = std::move(result);
    });
    ++i;
    ++first;
  }

  return state->p.getFuture();
}
}
