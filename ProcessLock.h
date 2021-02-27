#pragma once

#include "FileDescriptor.h"

#include <chrono>
#include <variant>

namespace watchman {

class ProcessLock {
 public:
  using LockError = std::string;

  /**
   * Move-only unit type that indicates the process lock has been acquired.
   */
  class Handle {
   public:
    Handle(Handle&&) = default;
    Handle& operator=(Handle&&) = default;

   private:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
  };

  /**
   * Acquires an fd to the pidfile and locks it.
   *
   * Call before fork(), so failure can be printed to the daemonizing process.
   *
   * Prints an error and exits the process if it fails.
   */
  static ProcessLock acquire(const std::string& pid_file);

  /**
   * Acquires an fd to the pidfile and locks it.
   *
   * Call before fork(), so failure can be printed to the daemonizing process.
   *
   * If it fails, it returns a string containing the error message.
   */
  static std::variant<ProcessLock, LockError> tryAcquire(
      const std::string& pid_file);

  ProcessLock(ProcessLock&&) = default;
  ProcessLock& operator=(ProcessLock&&) = default;

  ProcessLock(const ProcessLock&) = delete;
  ProcessLock& operator=(const ProcessLock&) = delete;

  /**
   * Called by the daemonized process to write the daemon pid into the locked
   * pidfile.
   *
   * This releases the FileDescriptor but does not close it, as the lock should
   * be held for the process's lifetime.
   */
  Handle writePid(const std::string& pid_file);

 private:
#ifndef _WIN32
  ProcessLock() = delete;
  explicit ProcessLock(FileDescriptor fd) : fd_{std::move(fd)} {}

  FileDescriptor fd_;
#else
  ProcessLock() = default;
#endif
};

} // namespace watchman
