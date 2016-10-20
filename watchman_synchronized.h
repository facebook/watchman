/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
/**
 * This module implements a Synchronized abstraction useful in
 * mutex-based concurrency.  It was borrowed from folly::Synchronized
 * but cut down a little so that watchman does not need to pull in all
 * of folly and its somewhat obscure dependencies.
 *
 * The Synchronized<T, Mutex> class is the primary public API exposed by this
 * module.  See folly/docs/Synchronized.md for a more complete explanation of
 * this class and its benefits.
 */

#include <mutex>
#include <type_traits>
#include "watchman_shared_mutex.h"

namespace watchman {

/*
 * Lock policy classes.
 *
 * These can be used as template parameters to provide compile-time
 * selection over the type of lock operation to perform.
 */

/**
 * A lock policy that performs exclusive lock operations.
 */
struct LockPolicyExclusive {
  template <class Mutex>
  static void lock(Mutex& mutex) {
    mutex.lock();
  }
  template <class Mutex, class Rep, class Period>
  static bool try_lock_for(
      Mutex& mutex,
      const std::chrono::duration<Rep, Period>& timeout) {
    return mutex.try_lock_for(timeout);
  }
  template <class Mutex>
  static void unlock(Mutex& mutex) {
    mutex.unlock();
  }
};

/**
 * A lock policy that performs shared lock operations.
 * This policy only works with shared mutex types.
 */
struct LockPolicyShared {
  template <class Mutex>
  static void lock(Mutex& mutex) {
    mutex.lock_shared();
  }
  template <class Mutex, class Rep, class Period>
  static bool try_lock_for(
      Mutex& mutex,
      const std::chrono::duration<Rep, Period>& timeout) {
    return mutex.try_lock_shared_for(mutex, timeout);
  }
  template <class Mutex>
  static void unlock(Mutex& mutex) {
    mutex.unlock_shared();
  }
};

template <class LockedType, class Mutex, class LockPolicy>
class LockedPtrBase;
template <class LockedType, class LockPolicy>
class LockedPtr;
template <class LockedType, class LockPolicy = LockPolicyExclusive>
class LockedGuardPtr;

/**
 * SynchronizedBase is a helper parent class for Synchronized<T>.
 *
 * It provides wlock() and rlock() methods for shared mutex types,
 * or lock() methods for purely exclusive mutex types.
 */
template <class Subclass>
class SynchronizedBase;

/**
 * SynchronizedBase specialization for shared mutex types.
 *
 * This class provides wlock() and rlock() methods for acquiring the lock and
 * accessing the data.
 */
template <class Subclass>
class SynchronizedBase {
 public:
  using LockedPtr = ::watchman::LockedPtr<Subclass, LockPolicyExclusive>;
  using ConstWLockedPtr =
      ::watchman::LockedPtr<const Subclass, LockPolicyExclusive>;
  using ConstLockedPtr =
      ::watchman::LockedPtr<const Subclass, LockPolicyShared>;

  /**
   * Acquire an exclusive lock, and return a LockedPtr that can be used to
   * safely access the datum.
   *
   * LockedPtr offers operator -> and * to provide access to the datum.
   * The lock will be released when the LockedPtr is destroyed.
   */
  LockedPtr wlock() {
    return LockedPtr(static_cast<Subclass*>(this));
  }
  ConstWLockedPtr wlock() const {
    return ConstWLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * Acquire a read lock, and return a ConstLockedPtr that can be used to
   * safely access the datum.
   */
  ConstLockedPtr rlock() const {
    return ConstLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::isNull() to check for validity.)
   */
  template <class Rep, class Period>
  LockedPtr wlock(const std::chrono::duration<Rep, Period>& timeout) {
    return LockedPtr(static_cast<Subclass*>(this), timeout);
  }
  template <class Rep, class Period>
  ConstWLockedPtr wlock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstWLockedPtr(static_cast<const Subclass*>(this), timeout);
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::isNull() to check for validity.)
   */
  template <class Rep, class Period>
  ConstLockedPtr rlock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(static_cast<const Subclass*>(this), timeout);
  }
};

/**
 * Synchronized<T> encapsulates an object of type T (a "datum") paired
 * with a mutex. The only way to access the datum is while the mutex
 * is locked, and Synchronized makes it virtually impossible to do
 * otherwise. The code that would access the datum in unsafe ways
 * would look odd and convoluted, thus readily alerting the human
 * reviewer. In contrast, the code that uses Synchronized<T> correctly
 * looks simple and intuitive.
 */
template <class T, class Mutex = shared_timed_mutex>
struct Synchronized : public SynchronizedBase<Synchronized<T, Mutex>> {
 private:
  using Base = SynchronizedBase<Synchronized<T, Mutex>>;
  static constexpr bool nxCopyCtor{
      std::is_nothrow_copy_constructible<T>::value};
  static constexpr bool nxMoveCtor{
      std::is_nothrow_move_constructible<T>::value};

 public:
  using LockedPtr = typename Base::LockedPtr;
  using ConstLockedPtr = typename Base::ConstLockedPtr;
  using DataType = T;
  using MutexType = Mutex;

  /**
   * Default constructor leaves both members call their own default
   * constructor.
   */
  Synchronized() = default;

  /**
   * Copy constructor copies the data (with locking the source and
   * all) but does NOT copy the mutex. Doing so would result in
   * deadlocks.
   *
   * Note that the copy constructor may throw because it acquires a lock in
   * the contextualRLock() method
   */
  Synchronized(const Synchronized& rhs) /* may throw */
      : Synchronized(rhs, rhs.contextualRLock()) {}

  /**
   * Move constructor moves the data (with locking the source and all)
   * but does not move the mutex.
   *
   * Note that the move constructor may throw because it acquires a lock.
   * Since the move constructor is not declared noexcept, when objects of this
   * class are used as elements in a vector or a similar container.  The
   * elements might not be moved around when resizing.  They might be copied
   * instead.  You have been warned.
   */
  Synchronized(Synchronized&& rhs) /* may throw */
      : Synchronized(std::move(rhs), rhs.contextualLock()) {}

  /**
   * Constructor taking a datum as argument copies it. There is no
   * need to lock the constructing object.
   */
  explicit Synchronized(const T& rhs) noexcept(nxCopyCtor) : datum_(rhs) {}

  /**
   * Constructor taking a datum rvalue as argument moves it. Again,
   * there is no need to lock the constructing object.
   */
  explicit Synchronized(T&& rhs) noexcept(nxMoveCtor)
      : datum_(std::move(rhs)) {}

  /**
   * The canonical assignment operator only assigns the data, NOT the
   * mutex. It locks the two objects in ascending order of their
   * addresses.
   */
  Synchronized& operator=(const Synchronized& rhs) {
    if (this == &rhs) {
      // Self-assignment, pass.
    } else if (this < &rhs) {
      auto guard1 = LockedPtr(this);
      auto guard2 = LockedPtr(&rhs);
      datum_ = rhs.datum_;
    } else {
      auto guard1 = LockedPtr(&rhs);
      auto guard2 = LockedPtr(this);
      datum_ = rhs.datum_;
    }
    return *this;
  }

  /**
   * Move assignment operator, only assigns the data, NOT the
   * mutex. It locks the two objects in ascending order of their
   * addresses.
   */
  Synchronized& operator=(Synchronized&& rhs) {
    if (this == &rhs) {
      // Self-assignment, pass.
    } else if (this < &rhs) {
      auto guard1 = LockedPtr(this);
      auto guard2 = LockedPtr(&rhs);
      datum_ = std::move(rhs.datum_);
    } else {
      auto guard1 = LockedPtr(&rhs);
      auto guard2 = LockedPtr(this);
      datum_ = std::move(rhs.datum_);
    }
    return *this;
  }

  /**
   * Lock object, assign datum.
   */
  Synchronized& operator=(const T& rhs) {
    auto guard = LockedPtr(this);
    datum_ = rhs;
    return *this;
  }

  /**
   * Lock object, move-assign datum.
   */
  Synchronized& operator=(T&& rhs) {
    auto guard = LockedPtr(this);
    datum_ = std::move(rhs);
    return *this;
  }

  /**
   * Acquire an appropriate lock based on the context.
   *
   * If the mutex is a shared mutex, and the Synchronized instance is const,
   * this acquires a shared lock.  Otherwise this acquires an exclusive lock.
   *
   * In general, prefer using the explicit rlock() and wlock() methods
   * for read-write locks, and lock() for purely exclusive locks.
   *
   * contextualLock() is primarily intended for use in other template functions
   * that do not necessarily know the lock type.
   */
  LockedPtr contextualLock() {
    return LockedPtr(this);
  }
  ConstLockedPtr contextualLock() const {
    return ConstLockedPtr(this);
  }
  template <class Rep, class Period>
  LockedPtr contextualLock(const std::chrono::duration<Rep, Period>& timeout) {
    return LockedPtr(this, timeout);
  }
  template <class Rep, class Period>
  ConstLockedPtr contextualLock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(this, timeout);
  }
  /**
   * contextualRLock() acquires a read lock if the mutex type is shared,
   * or a regular exclusive lock for non-shared mutex types.
   *
   * contextualRLock() when you know that you prefer a read lock (if
   * available), even if the Synchronized<T> object itself is non-const.
   */
  ConstLockedPtr contextualRLock() const {
    return ConstLockedPtr(this);
  }
  template <class Rep, class Period>
  ConstLockedPtr contextualRLock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(this, timeout);
  }

  /**
   * Swaps with another Synchronized. Protected against
   * self-swap. Only data is swapped. Locks are acquired in increasing
   * address order.
   */
  void swap(Synchronized& rhs) {
    if (this == &rhs) {
      return;
    }
    if (this > &rhs) {
      return rhs.swap(*this);
    }
    auto guard1 = LockedPtr(this);
    auto guard2 = LockedPtr(&rhs);

    using std::swap;
    swap(datum_, rhs.datum_);
  }

  /**
   * Swap with another datum. Recommended because it keeps the mutex
   * held only briefly.
   */
  void swap(T& rhs) {
    LockedPtr guard(this);

    using std::swap;
    swap(datum_, rhs);
  }

  /**
   * Copies datum to a given target.
   */
  void copy(T* target) const {
    ConstLockedPtr guard(this);
    *target = datum_;
  }

  /**
   * Returns a fresh copy of the datum.
   */
  T copy() const {
    ConstLockedPtr guard(this);
    return datum_;
  }

 private:
  template <class LockedType, class MutexType, class LockPolicy>
  friend class watchman::LockedPtrBase;
  template <class LockedType, class LockPolicy>
  friend class watchman::LockedPtr;
  template <class LockedType, class LockPolicy>
  friend class watchman::LockedGuardPtr;

  /**
   * Helper constructors to enable Synchronized for
   * non-default constructible types T.
   * Guards are created in actual public constructors and are alive
   * for the time required to construct the object
   */
  Synchronized(
      const Synchronized& rhs,
      const ConstLockedPtr& /*guard*/) noexcept(nxCopyCtor)
      : datum_(rhs.datum_) {}

  Synchronized(Synchronized&& rhs, const LockedPtr& /*guard*/) noexcept(
      nxMoveCtor)
      : datum_(std::move(rhs.datum_)) {}

  // Synchronized data members
  T datum_;
  mutable Mutex mutex_;
};

template <class SynchronizedType, class LockPolicy>
class ScopedUnlocker;

namespace detail {
/*
 * A helper alias that resolves to "const T" if the template parameter
 * is a const Synchronized<T>, or "T" if the parameter is not const.
 */
template <class SynchronizedType>
using SynchronizedDataType = typename std::conditional<
    std::is_const<SynchronizedType>::value,
    typename SynchronizedType::DataType const,
    typename SynchronizedType::DataType>::type;
/*
 * A helper alias that resolves to a ConstLockedPtr if the template parameter
 * is a const Synchronized<T>, or a LockedPtr if the parameter is not const.
 */
template <class SynchronizedType>
using LockedPtrType = typename std::conditional<
    std::is_const<SynchronizedType>::value,
    typename SynchronizedType::ConstLockedPtr,
    typename SynchronizedType::LockedPtr>::type;
} // detail

/**
 * A helper base class for implementing LockedPtr.
 *
 * The main reason for having this as a separate class is so we can specialize
 * it for std::mutex, so we can expose a std::unique_lock to the caller
 * when std::mutex is being used.  This allows callers to use a
 * std::condition_variable with the mutex from a Synchronized<T, std::mutex>.
 *
 * We don't use std::unique_lock with other Mutex types since it makes the
 * LockedPtr class slightly larger, and it makes the logic to support
 * ScopedUnlocker slightly more complicated.  std::mutex is the only one that
 * really seems to benefit from the unique_lock.  std::condition_variable
 * itself only supports std::unique_lock<std::mutex>, so there doesn't seem to
 * be any real benefit to exposing the unique_lock with other mutex types.
 *
 * Note that the SynchronizedType template parameter may or may not be const
 * qualified.
 */
template <class SynchronizedType, class Mutex, class LockPolicy>
class LockedPtrBase {
 public:
  using MutexType = Mutex;
  friend class watchman::ScopedUnlocker<SynchronizedType, LockPolicy>;

  /**
   * Destructor releases.
   */
  ~LockedPtrBase() {
    if (parent_) {
      LockPolicy::unlock(parent_->mutex_);
    }
  }

  /**
   * Unlock the synchronized data.
   *
   * The LockedPtr can no longer be dereferenced after unlock() has been
   * called.  isValid() will return false on an unlocked LockedPtr.
   *
   * unlock() can only be called on a LockedPtr that is valid.
   */
  void unlock() {
    // DCHECK(parent_ != nullptr);
    LockPolicy::unlock(parent_->mutex_);
    parent_ = nullptr;
  }

 protected:
  LockedPtrBase() {}
  explicit LockedPtrBase(SynchronizedType* parent) : parent_(parent) {
    LockPolicy::lock(parent_->mutex_);
  }
  template <class Rep, class Period>
  LockedPtrBase(
      SynchronizedType* parent,
      const std::chrono::duration<Rep, Period>& timeout) {
    if (LockPolicy::try_lock_for(parent->mutex_, timeout)) {
      this->parent_ = parent;
    }
  }
  LockedPtrBase(LockedPtrBase&& rhs) noexcept : parent_(rhs.parent_) {
    rhs.parent_ = nullptr;
  }
  LockedPtrBase& operator=(LockedPtrBase&& rhs) noexcept {
    if (parent_) {
      LockPolicy::unlock(parent_->mutex_);
    }

    parent_ = rhs.parent_;
    rhs.parent_ = nullptr;
    return *this;
  }

  using UnlockerData = SynchronizedType*;

  /**
   * Get a pointer to the Synchronized object from the UnlockerData.
   *
   * In the generic case UnlockerData is just the Synchronized pointer,
   * so we return it as is.  (This function is more interesting in the
   * std::mutex specialization below.)
   */
  static SynchronizedType* getSynchronized(UnlockerData data) {
    return data;
  }

  UnlockerData releaseLock() {
    // DCHECK(parent_ != nullptr);
    auto current = parent_;
    parent_ = nullptr;
    LockPolicy::unlock(current->mutex_);
    return current;
  }
  void reacquireLock(UnlockerData&& data) {
    // DCHECK(parent_ == nullptr);
    parent_ = data;
    LockPolicy::lock(parent_->mutex_);
  }

  SynchronizedType* parent_ = nullptr;
};

/**
 * LockedPtrBase specialization for use with std::mutex.
 *
 * When std::mutex is used we use a std::unique_lock to hold the mutex.
 * This makes it possible to use std::condition_variable with a
 * Synchronized<T, std::mutex>.
 */
template <class SynchronizedType, class LockPolicy>
class LockedPtrBase<SynchronizedType, std::mutex, LockPolicy> {
 public:
  using MutexType = std::mutex;
  friend class watchman::ScopedUnlocker<SynchronizedType, LockPolicy>;

  /**
   * Destructor releases.
   */
  ~LockedPtrBase() {
    // The std::unique_lock will automatically release the lock when it is
    // destroyed, so we don't need to do anything extra here.
  }

  LockedPtrBase(LockedPtrBase&& rhs) noexcept
      : lock_(std::move(rhs.lock_)), parent_(rhs.parent_) {
    rhs.parent_ = nullptr;
  }
  LockedPtrBase& operator=(LockedPtrBase&& rhs) noexcept {
    lock_ = std::move(rhs.lock_);
    parent_ = rhs.parent_;
    rhs.parent_ = nullptr;
    return *this;
  }

  /**
   * Get a reference to the std::unique_lock.
   *
   * This is provided so that callers can use Synchronized<T, std::mutex>
   * with a std::condition_variable.
   *
   * While this API could be used to bypass the normal Synchronized APIs and
   * manually interact with the underlying unique_lock, this is strongly
   * discouraged.
   */
  std::unique_lock<std::mutex>& getUniqueLock() {
    return lock_;
  }

  /**
   * Unlock the synchronized data.
   *
   * The LockedPtr can no longer be dereferenced after unlock() has been
   * called.  isValid() will return false on an unlocked LockedPtr.
   *
   * unlock() can only be called on a LockedPtr that is valid.
   */
  void unlock() {
    // DCHECK(parent_ != nullptr);
    lock_.unlock();
    parent_ = nullptr;
  }

 protected:
  LockedPtrBase() {}
  explicit LockedPtrBase(SynchronizedType* parent)
      : lock_(parent->mutex_), parent_(parent) {}

  using UnlockerData =
      std::pair<std::unique_lock<std::mutex>, SynchronizedType*>;

  static SynchronizedType* getSynchronized(const UnlockerData& data) {
    return data.second;
  }

  UnlockerData releaseLock() {
    // DCHECK(parent_ != nullptr);
    UnlockerData data(std::move(lock_), parent_);
    parent_ = nullptr;
    data.first.unlock();
    return data;
  }
  void reacquireLock(UnlockerData&& data) {
    lock_ = std::move(data.first);
    lock_.lock();
    parent_ = data.second;
  }

  // The specialization for std::mutex does have to store slightly more
  // state than the default implementation.
  std::unique_lock<std::mutex> lock_;
  SynchronizedType* parent_ = nullptr;
};

/**
 * This class temporarily unlocks a LockedPtr in a scoped manner.
 */
template <class SynchronizedType, class LockPolicy>
class ScopedUnlocker {
 public:
  explicit ScopedUnlocker(LockedPtr<SynchronizedType, LockPolicy>* p)
      : ptr_(p), data_(ptr_->releaseLock()) {}
  ScopedUnlocker(const ScopedUnlocker&) = delete;
  ScopedUnlocker& operator=(const ScopedUnlocker&) = delete;
  ScopedUnlocker(ScopedUnlocker&& other) noexcept
      : ptr_(other.ptr_), data_(std::move(other.data_)) {
    other.ptr_ = nullptr;
  }
  ScopedUnlocker& operator=(ScopedUnlocker&& other) = delete;

  ~ScopedUnlocker() {
    if (ptr_) {
      ptr_->reacquireLock(std::move(data_));
    }
  }

  /**
   * Return a pointer to the Synchronized object used by this ScopedUnlocker.
   */
  SynchronizedType* getSynchronized() const {
    return LockedPtr<SynchronizedType, LockPolicy>::getSynchronized(data_);
  }

 private:
  using Data = typename LockedPtr<SynchronizedType, LockPolicy>::UnlockerData;
  LockedPtr<SynchronizedType, LockPolicy>* ptr_{nullptr};
  Data data_;
};

/**
 * A LockedPtr keeps a Synchronized<T> object locked for the duration of
 * LockedPtr's existence.
 *
 * It provides access the datum's members directly by using operator->() and
 * operator*().
 *
 * The LockPolicy parameter controls whether or not the lock is acquired in
 * exclusive or shared mode.
 */
template <class SynchronizedType, class LockPolicy>
class LockedPtr : public LockedPtrBase<
                      SynchronizedType,
                      typename SynchronizedType::MutexType,
                      LockPolicy> {
 private:
  using Base = LockedPtrBase<
      SynchronizedType,
      typename SynchronizedType::MutexType,
      LockPolicy>;
  using UnlockerData = typename Base::UnlockerData;
  // CDataType is the DataType with the appropriate const-qualification
  using CDataType = detail::SynchronizedDataType<SynchronizedType>;

 public:
  using DataType = typename SynchronizedType::DataType;
  using MutexType = typename SynchronizedType::MutexType;
  using Synchronized = typename std::remove_const<SynchronizedType>::type;
  friend class ScopedUnlocker<SynchronizedType, LockPolicy>;

  /**
   * Creates an uninitialized LockedPtr.
   *
   * Dereferencing an uninitialized LockedPtr is not allowed.
   */
  LockedPtr() {}

  /**
   * Takes a Synchronized<T> and locks it.
   */
  explicit LockedPtr(SynchronizedType* parent) : Base(parent) {}

  /**
   * Takes a Synchronized<T> and attempts to lock it, within the specified
   * timeout.
   *
   * Blocks until the lock is acquired or until the specified timeout expires.
   * If the timeout expired without acquiring the lock, the LockedPtr will be
   * null, and LockedPtr::isNull() will return true.
   */
  template <class Rep, class Period>
  LockedPtr(
      SynchronizedType* parent,
      const std::chrono::duration<Rep, Period>& timeout)
      : Base(parent, timeout) {}

  /**
   * Move constructor.
   */
  LockedPtr(LockedPtr&&) noexcept = default;

  /**
   * Move assignment operator.
   */
  LockedPtr& operator=(LockedPtr&& rhs) noexcept = default;

  /*
   * Copy constructor and assignment operator are deleted.
   */
  LockedPtr(const LockedPtr& rhs) = delete;
  LockedPtr& operator=(const LockedPtr& rhs) = delete;

  /**
   * Destructor releases.
   */
  ~LockedPtr() {}

  /**
   * Check if this LockedPtr is uninitialized, or points to valid locked data.
   *
   * This method can be used to check if a timed-acquire operation succeeded.
   * If an acquire operation times out it will result in a null LockedPtr.
   *
   * A LockedPtr is always either null, or holds a lock to valid data.
   * Methods such as scopedUnlock() reset the LockedPtr to null for the
   * duration of the unlock.
   */
  bool isNull() const {
    return this->parent_ == nullptr;
  }

  /**
   * Explicit boolean conversion.
   *
   * Returns !isNull()
   */
  explicit operator bool() const {
    return this->parent_ != nullptr;
  }

  /**
   * Access the locked data.
   *
   * This method should only be used if the LockedPtr is valid.
   */
  CDataType* operator->() const {
    return &this->parent_->datum_;
  }

  /**
   * Access the locked data.
   *
   * This method should only be used if the LockedPtr is valid.
   */
  CDataType& operator*() const {
    return this->parent_->datum_;
  }

  /**
   * Temporarily unlock the LockedPtr, and reset it to null.
   *
   * Returns an helper object that will re-lock and restore the LockedPtr when
   * the helper is destroyed.  The LockedPtr may not be dereferenced for as
   * long as this helper object exists.
   */
  ScopedUnlocker<SynchronizedType, LockPolicy> scopedUnlock() {
    return ScopedUnlocker<SynchronizedType, LockPolicy>(this);
  }
};

/**
 * LockedGuardPtr is a simplified version of LockedPtr.
 *
 * It is non-movable, and supports fewer features than LockedPtr.  However, it
 * is ever-so-slightly more performant than LockedPtr.  (The destructor can
 * unconditionally release the lock, without requiring a conditional branch.)
 *
 * The relationship between LockedGuardPtr and LockedPtr is similar to that
 * between std::lock_guard and std::unique_lock.
 */
template <class SynchronizedType, class LockPolicy>
class LockedGuardPtr {
 private:
  // CDataType is the DataType with the appropriate const-qualification
  using CDataType = detail::SynchronizedDataType<SynchronizedType>;

 public:
  using DataType = typename SynchronizedType::DataType;
  using MutexType = typename SynchronizedType::MutexType;
  using Synchronized = typename std::remove_const<SynchronizedType>::type;

  LockedGuardPtr() = delete;

  /**
   * Takes a Synchronized<T> and locks it.
   */
  explicit LockedGuardPtr(SynchronizedType* parent) : parent_(parent) {
    LockPolicy::lock(parent_->mutex_);
  }

  /**
   * Destructor releases.
   */
  ~LockedGuardPtr() {
    LockPolicy::unlock(parent_->mutex_);
  }

  /**
   * Access the locked data.
   */
  CDataType* operator->() const {
    return &parent_->datum_;
  }

  /**
   * Access the locked data.
   */
  CDataType& operator*() const {
    return parent_->datum_;
  }

 private:
  // This is the entire state of LockedGuardPtr.
  SynchronizedType* const parent_{nullptr};
};

/**
 * Acquire locks for multiple Synchronized<T> objects, in a deadlock-safe
 * manner.
 *
 * The locks are acquired in order from lowest address to highest address.
 * (Note that this is not necessarily the same algorithm used by std::lock().)
 *
 * For parameters that are const and support shared locks, a read lock is
 * acquired.  Otherwise an exclusive lock is acquired.
 *
 * TODO: Extend acquireLocked() with variadic template versions that
 * allow for more than 2 Synchronized arguments.  (I haven't given too much
 * thought about how to implement this.  It seems like it would be rather
 * complicated, but I think it should be possible.)
 */
template <class Sync1, class Sync2>
std::tuple<detail::LockedPtrType<Sync1>, detail::LockedPtrType<Sync2>>
acquireLocked(Sync1& l1, Sync2& l2) {
  if (static_cast<const void*>(&l1) < static_cast<const void*>(&l2)) {
    auto p1 = l1.contextualLock();
    auto p2 = l2.contextualLock();
    return std::make_tuple(std::move(p1), std::move(p2));
  } else {
    auto p2 = l2.contextualLock();
    auto p1 = l1.contextualLock();
    return std::make_tuple(std::move(p1), std::move(p2));
  }
}

/**
 * A version of acquireLocked() that returns a std::pair rather than a
 * std::tuple, which is easier to use in many places.
 */
template <class Sync1, class Sync2>
std::pair<detail::LockedPtrType<Sync1>, detail::LockedPtrType<Sync2>>
acquireLockedPair(Sync1& l1, Sync2& l2) {
  auto lockedPtrs = acquireLocked(l1, l2);
  return {std::move(std::get<0>(lockedPtrs)),
          std::move(std::get<1>(lockedPtrs))};
}

} // namespace watchman
