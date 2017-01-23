/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "ChildProcess.h"
#include <system_error>
#include "Logging.h"
#include "make_unique.h"
#include "watchman_env.h"
#include "watchman_scopeguard.h"

namespace watchman {

ChildProcess::Environment::Environment() : map_(w_envp_make_ht()) {}

ChildProcess::Environment::Environment(
    const std::unordered_map<w_string, w_string>& map)
    : map_(map) {}

std::unique_ptr<char*, ChildProcess::Deleter>
ChildProcess::Environment::asEnviron(size_t* env_size) const {
  uint32_t size = 0;
  auto result = std::unique_ptr<char*, Deleter>(
      w_envp_make_from_ht(map_, &size), Deleter());
  if (env_size) {
    *env_size = size;
  }
  return result;
}

void ChildProcess::Environment::set(const w_string& key, const w_string& val) {
  map_[key] = val;
}

void ChildProcess::Environment::set(
    std::initializer_list<std::pair<w_string_piece, w_string_piece>> pairs) {
  for (auto& pair : pairs) {
    set(pair.first.asWString(), pair.second.asWString());
  }
}

void ChildProcess::Environment::unset(const w_string& key) {
  map_.erase(key);
}

ChildProcess::Options::Options() : inner_(make_unique<Inner>()) {
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  setFlags(POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
}

ChildProcess::Options::Inner::Inner() {
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_init(&actions);
}

ChildProcess::Options::Inner::~Inner() {
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
}

void ChildProcess::Options::setFlags(short flags) {
  short currentFlags;
  auto err = posix_spawnattr_getflags(&inner_->attr, &currentFlags);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawnattr_getflags");
  }
  err = posix_spawnattr_setflags(&inner_->attr, currentFlags | flags);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawnattr_setflags");
  }
}

#ifdef POSIX_SPAWN_SETSIGMASK
void ChildProcess::Options::setSigMask(const sigset_t& mask) {
  posix_spawnattr_setsigmask(&inner_->attr, &mask);
  setFlags(POSIX_SPAWN_SETSIGMASK);
}
#endif

ChildProcess::Environment& ChildProcess::Options::environment() {
  return env_;
}

void ChildProcess::Options::dup2(int fd, int targetFd) {
  auto err = posix_spawn_file_actions_adddup2(&inner_->actions, fd, targetFd);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawn_file_actions_adddup2");
  }
}

#ifdef _WIN32
void ChildProcess::Options::dup2(intptr_t handle, int targetFd) {
  auto err = posix_spawn_file_actions_adddup2_handle_np(
      &inner_->actions, handle, targetFd);
  if (err) {
    throw std::system_error(
        err,
        std::generic_category(),
        "posix_spawn_file_actions_adddup2_handle_np");
  }
}
#endif

void ChildProcess::Options::open(
    int targetFd,
    const char* path,
    int flags,
    int mode) {
  auto err = posix_spawn_file_actions_addopen(
      &inner_->actions, targetFd, path, flags, mode);
  if (err) {
    throw std::system_error(
        err, std::generic_category(), "posix_spawn_file_actions_addopen");
  }
}

void ChildProcess::Options::pipe(int targetFd, bool childRead) {
  if (pipes_.find(targetFd) != pipes_.end()) {
    throw std::runtime_error("targetFd is already present in pipes map");
  }

  auto result = pipes_.emplace(std::make_pair(targetFd, make_unique<Pipe>()));
  auto pipe = result.first->second.get();

  dup2(childRead ? pipe->read.fd() : pipe->write.fd(), targetFd);
}

void ChildProcess::Options::pipeStdin() {
  pipe(STDIN_FILENO, true);
}

void ChildProcess::Options::pipeStdout() {
  pipe(STDOUT_FILENO, false);
}

void ChildProcess::Options::pipeStderr() {
  pipe(STDERR_FILENO, false);
}

void ChildProcess::Options::chdir(w_string_piece path) {
  cwd_ = std::string(path.data(), path.size());
#ifdef _WIN32
  posix_spawnattr_setcwd_np(&inner_->attr, cwd_.c_str());
#endif
}

static std::vector<w_string_piece> json_args_to_string_vec(
    const json_ref& args) {
  std::vector<w_string_piece> vec;

  for (auto& arg : args.array()) {
    vec.emplace_back(json_to_w_string(arg));
  }

  return vec;
}

ChildProcess::ChildProcess(const json_ref& args, Options&& options)
    : ChildProcess(json_args_to_string_vec(args), std::move(options)) {}

ChildProcess::ChildProcess(std::vector<w_string_piece> args, Options&& options)
    : pipes_(std::move(options.pipes_)) {
  std::vector<char*> argv;
  std::vector<std::string> argStrings;

  argStrings.reserve(args.size());
  argv.reserve(args.size() + 1);

  for (auto& str : args) {
    argStrings.emplace_back(str.data(), str.size());
    argv.emplace_back(&argStrings.back()[0]);
  }
  argv.emplace_back(nullptr);

#ifndef _WIN32
  auto lock = lockCwdMutex();
  char savedCwd[WATCHMAN_NAME_MAX];
  if (!getcwd(savedCwd, sizeof(savedCwd))) {
    throw std::system_error(errno, std::generic_category(), "failed to getcwd");
  }
  SCOPE_EXIT {
    if (!options.cwd_.empty()) {
      if (chdir(savedCwd) != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            watchman::to<std::string>("failed to chdir to ", savedCwd));
      }
    }
  };

  if (!options.cwd_.empty()) {
    if (chdir(options.cwd_.c_str()) != 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          watchman::to<std::string>("failed to chdir to ", options.cwd_));
    }
  }
#endif

  auto envp = options.env_.asEnviron();
  auto ret = posix_spawnp(
      &pid_,
      argv[0],
      &options.inner_->actions,
      &options.inner_->attr,
      &argv[0],
      envp.get());

  if (ret) {
    // Failed, so the creator cannot call wait() on us.
    // mark us as already done.
    waited_ = true;
  }

  // Log some info
  auto level = ret == 0 ? watchman::DBG : watchman::ERR;
  watchman::log(level, "ChildProcess: pid=", pid_, "\n");
  for (size_t i = 0; i < args.size(); ++i) {
    watchman::log(level, "argv[", i, "] ", args[i], "\n");
  }
  for (size_t i = 0; envp.get()[i]; ++i) {
    watchman::log(level, "envp[", i, "] ", envp.get()[i], "\n");
  }

  if (ret) {
    throw std::system_error(ret, std::generic_category(), "posix_spawnp");
  }
}

static std::mutex& getCwdMutex() {
  // Meyers singleton
  static std::mutex m;
  return m;
}

std::unique_lock<std::mutex> ChildProcess::lockCwdMutex() {
  return std::unique_lock<std::mutex>(getCwdMutex());
}

ChildProcess::~ChildProcess() {
  if (!waited_) {
    watchman::log(
        watchman::FATAL,
        "you must call ChildProcess.wait() before destroying a ChildProcess\n");
  }
}

void ChildProcess::disown() {
  waited_ = true;
}

bool ChildProcess::terminated() {
  if (waited_) {
    return true;
  }

  auto pid = waitpid(pid_, &status_, WNOHANG);
  if (pid == pid_) {
    waited_ = true;
  }

  return waited_;
}

int ChildProcess::wait() {
  if (waited_) {
    return status_;
  }

  while (true) {
    auto pid = waitpid(pid_, &status_, 0);
    if (pid == pid_) {
      waited_ = true;
      return status_;
    }

    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(), "waitpid");
    }
  }
}

void ChildProcess::kill(
#ifndef _WIN32
    int signo
#endif
    ) {
#ifndef _WIN32
  if (!waited_) {
    ::kill(pid_, signo);
  }
#endif
}
}
