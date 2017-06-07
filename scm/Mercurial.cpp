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
  bool result;

  try {
    auto info = getFileInformation(dirStatePath.c_str(),
                                   CaseSensitivity::CaseSensitive);

    if (info.size != dirstate.size ||
        info.mtime.tv_sec != dirstate.mtime.tv_sec ||
        info.mtime.tv_nsec != dirstate.mtime.tv_nsec) {
      log(DBG, "mergeBases stat(", dirStatePath, ") info differs\n");
      result = true;
    } else {
      result = false;
      log(DBG, "mergeBases stat(", dirStatePath, ") info same\n");
    }

    dirstate = info;

  } catch (const std::system_error &exc) {
    // Failed to stat, so assume that it changed
    log(DBG, "mergeBases stat(", dirStatePath, ") failed: ", exc.what(), "\n");
    result = true;
  }
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
  opt.nullStdin();
  opt.pipeStdout();
  opt.pipeStderr();
  opt.chdir(getRootPath());

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {"hg", "log", "-T", "{node}", "-r", revset}, std::move(opt));

  auto outputs = proc.communicate();
  auto status = proc.wait();

  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the merge base; command returned with status ",
        status,
        ", output=",
        outputs.first,
        " error=",
        outputs.second));
  }

  if (outputs.first.size() != 40) {
    throw std::runtime_error(to<std::string>(
        "expected merge base to be a 40 character string, got ",
        outputs.first));
  }

  {
    auto cache = cache_.wlock();
    cache->mergeBases[idString] = outputs.first;
  }
  return outputs.first;
}

std::vector<w_string> Mercurial::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId) const {
  Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", w_string("1"));
  opt.nullStdin();
  opt.pipeStdout();
  opt.pipeStderr();
  opt.chdir(getRootPath());

  // The "" argument at the end causes paths to be printed out relative to the
  // cwd (set to root path above).
  ChildProcess proc(
      {"hg", "status", "-n", "--rev", commitId, ""}, std::move(opt));

  auto outputs = proc.communicate();

  std::vector<w_string> lines;
  w_string_piece(outputs.first).split(lines, '\n');

  auto status = proc.wait();
  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the hg status; command returned with status ",
        status,
        " out=",
        outputs.first,
        " err=",
        outputs.second));
  }

  return lines;
}
}
