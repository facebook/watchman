/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "Mercurial.h"
#include "ChildProcess.h"
#include "Logging.h"
#include "watchman.h"

// Capability indicating support for the mercurial SCM
W_CAP_REG("scm-hg")

namespace watchman {

using Options = ChildProcess::Options;

Mercurial::infoCache::infoCache(std::string path) : dirStatePath(path) {
  memset(&dirstate, 0, sizeof(dirstate));
}

w_string Mercurial::infoCache::lookupMergeBase(const std::string& commitId) {
  if (dotChanged()) {
    watchman::log(
        watchman::DBG, "Blowing mergeBases cache because dirstate changed\n");

    mergeBases.clear();
    return nullptr;
  }

  auto it = mergeBases.find(commitId);
  if (it != mergeBases.end()) {
    watchman::log(watchman::DBG, "mergeBases cache hit for ", commitId, "\n");
    return it->second;
  }
  watchman::log(watchman::DBG, "mergeBases cache miss for ", commitId, "\n");

  return nullptr;
}

bool Mercurial::infoCache::dotChanged() {
  struct stat st;
  bool result;

  if (lstat(dirStatePath.c_str(), &st)) {
    memset(&st, 0, sizeof(st));
    // Failed to stat, so assume that it changed
    watchman::log(
        watchman::DBG, "mergeBases stat(", dirStatePath, ") failed\n");
    result = true;
  } else if (
      st.st_size != dirstate.st_size ||
      st.WATCHMAN_ST_TIMESPEC(m).tv_sec !=
          dirstate.WATCHMAN_ST_TIMESPEC(m).tv_sec ||
      st.WATCHMAN_ST_TIMESPEC(m).tv_nsec !=
          dirstate.WATCHMAN_ST_TIMESPEC(m).tv_nsec) {
    watchman::log(
        watchman::DBG, "mergeBases stat(", dirStatePath, ") info differs\n");
    result = true;
  } else {
    result = false;
    watchman::log(
        watchman::DBG, "mergeBases stat(", dirStatePath, ") info same\n");
  }

  memcpy(&dirstate, &st, sizeof(st));

  return result;
}

Mercurial::Mercurial(w_string_piece rootPath, w_string_piece scmRoot)
    : SCM(rootPath, scmRoot),
      cache_(infoCache(to<std::string>(getSCMRoot(), "/.hg/dirstate"))) {}

w_string Mercurial::mergeBaseWith(w_string_piece commitId) const {
  std::string idString(commitId.data(), commitId.size());

  {
    auto cache = cache_.wlock();
    auto result = cache->lookupMergeBase(idString);
    if (result) {
      watchman::log(
          watchman::DBG,
          "Using cached mergeBase value of ",
          result,
          " for commitId ",
          commitId,
          " because dirstate file is unchanged\n");
      return result;
    }
  }

  Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", w_string("1"));
  opt.pipeStdout();
  opt.chdir(getRootPath());

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {"hg", "log", "-T", "{node}", "-r", revset}, std::move(opt));

  proc.pipe(STDOUT_FILENO).read.clearNonBlock();
  auto status = proc.wait();

  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the merge base; command returned with status ",
        status));
  }

  char buf[128];
  auto len = read(proc.pipe(STDOUT_FILENO).read.fd(), buf, sizeof(buf));

  if (len != 40) {
    throw std::runtime_error(to<std::string>(
        "expected merge base to be a 40 character string, got ",
        w_string_piece(buf, len)));
  }

  auto result = w_string(buf, len);
  {
    auto cache = cache_.wlock();
    cache->mergeBases[idString] = result;
  }
  return result;
}

std::vector<w_string> Mercurial::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId) const {
  Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", w_string("1"));
  opt.pipeStdout();
  opt.chdir(getRootPath());

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {"hg", "status", "-n", "--root-relative", "--rev", commitId},
      std::move(opt));

  proc.pipe(STDOUT_FILENO).read.clearNonBlock();
  std::vector<w_string> res;

  std::string line;
  char buf[512];

  while (true) {
    auto len = read(proc.pipe(STDOUT_FILENO).read.fd(), buf, sizeof(buf));
    if (len == 0) {
      break;
    }
    if (len < 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          "error while reading from hg status pipe");
    }
    auto shortRead = len < int(sizeof(buf));

    line.append(buf, len);

    // Note: mercurial always ends the last line with a newline,
    // so we only need do line splitting here.
    size_t start = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\n') {
        len = i - start;
        if (len > 0) {
          res.emplace_back(&line[start], len);
        }
        start = i + 1;
      }
    }

    if (start > 0) {
      line.erase(0, start);
    }

    if (shortRead) {
      // A short read means that we got to the end of the pipe data
      break;
    }
  }

  w_check(line.empty(), "we should have consumed all the data");

  auto status = proc.wait();
  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the hg status; command returned with status ",
        status));
  }

  return res;
}
}
