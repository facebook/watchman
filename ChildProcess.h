/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include "watchman_system.h"
#include <spawn.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Pipe.h"
#include "thirdparty/jansson/jansson.h"
#include "watchman_string.h"

namespace watchman {

class ChildProcess {
 public:
  struct Deleter {
    void operator()(char** vec) const {
      free((void*)vec);
    }
  };

  class Environment {
   public:
    // Constructs an environment from the current process environment
    Environment();
    Environment(const Environment&) = default;
    /* implicit */ Environment(
        const std::unordered_map<w_string, w_string>& map);

    Environment& operator=(const Environment&) = default;

    // Returns the environment as an environ compatible array
    std::unique_ptr<char*, Deleter> asEnviron(size_t* env_size = nullptr) const;

    // Set a value in the environment
    void set(const w_string& key, const w_string& value);
    void set(
        std::initializer_list<std::pair<w_string_piece, w_string_piece>> pairs);

    // Remove a value from the environment
    void unset(const w_string& key);

   private:
    std::unordered_map<w_string, w_string> map_;
  };

  class Options {
   public:
    Options();
    // Not copyable
    Options(const Options&) = delete;
    Options(Options&&) = default;
    Options& operator=(const Options&) = delete;
    Options& operator=(Options&&) = default;

#ifdef POSIX_SPAWN_SETSIGMASK
    void setSigMask(const sigset_t& mask);
#endif
    // Adds flags to the set of flags maintainted in the spawn attributes.
    // This is logically equivalent to calling setflags(getflags()|flags)
    void setFlags(short flags);

    Environment& environment();

    // Arranges to duplicate an fd from the parent as targetFd in
    // the child process.
    void dup2(int fd, int targetFd);
#ifdef _WIN32
    // Arranges to duplicate a windows handle from the parent as targetFd in
    // the child process.
    void dup2(intptr_t handle, int targetFd);
#endif

    // Arranges to create a pipe for communicating between the
    // parent and child process and setting it as targetFd in
    // the child.
    void pipe(int targetFd, bool childRead);

    // Set up stdin with a pipe
    void pipeStdin();

    // Set up stdout with a pipe
    void pipeStdout();

    // Set up stderr with a pipe
    void pipeStderr();

    // Arrange to open(2) a file for the child process and make
    // it available as targetFd
    void open(int targetFd, const char* path, int flags, int mode);

    // Arrange to set the cwd for the child process
    void chdir(w_string_piece path);

   private:
    struct Inner {
      // There is no defined way to copy or move either of
      // these things, so we separate them out into a container
      // that we can point to and move the pointer.
      posix_spawn_file_actions_t actions;
      posix_spawnattr_t attr;

      Inner();
      ~Inner();
    };
    std::unique_ptr<Inner> inner_;
    Environment env_;
    std::unordered_map<int, std::unique_ptr<Pipe>> pipes_;
    std::string cwd_;

    friend class ChildProcess;
  };

  ChildProcess(std::vector<w_string_piece> args, Options&& options);
  ChildProcess(const json_ref& args, Options&& options);
  ~ChildProcess();

  // Check to see if the process has terminated.
  // Does not block.  Returns true if the process has
  // terminated, false otherwise.
  bool terminated();

  // Wait for the process to terminate and return its
  // exit status.  If the process has already terminated,
  // immediately returns its exit status.
  int wait();

  // This mutex is present to avoid fighting over the cwd when multiple
  // process might need to chdir concurrently
  static std::unique_lock<std::mutex> lockCwdMutex();

  // Terminates the process
  void kill(
#ifndef _WIN32
      int signo = SIGTERM
#endif
      );

 private:
  pid_t pid_;
  bool waited_{false};
  int status_;
  std::unordered_map<int, std::unique_ptr<Pipe>> pipes_;
};
}
