#pragma once

namespace facebook::eden {

class RefCounted {
 public:
  virtual ~RefCounted() = default;
};

template <typename T>
class RefPtr {
 public:
  RefPtr() = default;
  explicit RefPtr(T* ptr) : ptr_(ptr) {}

  static RefPtr singleton(T& value) {
    return RefPtr(&value);
  }

  T* get() const {
    return ptr_;
  }

  T& operator*() const {
    return *ptr_;
  }

  T* operator->() const {
    return ptr_;
  }

 private:
  T* ptr_{nullptr};
};

} // namespace facebook::eden
