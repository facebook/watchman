/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "Mercurial.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include "ChildProcess.h"
#include "Logging.h"
#include "watchman.h"

// Capability indicating support for the mercurial SCM
W_CAP_REG("scm-hg")

using namespace std::chrono;

namespace watchman {

std::string hgExecutablePath() {
  auto hg = getenv("EDEN_HG_BINARY");
  if (hg && strlen(hg) > 0) {
    return hg;
  }
  return "hg";
}

ChildProcess::Options Mercurial::makeHgOptions(w_string requestId) const {
  ChildProcess::Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", w_string("1"));
  // chg can elect to kill all children if an error occurs in any child.
  // This can cause commands we spawn to fail transiently.  While we'd
  // love to have the lowest latency, the transient failure causes problems
  // with our ability to deliver notifications to our clients in a timely
  // manner, so we disable the use of chg for the mercurial processes
  // that we spawn.
  opt.environment().set("CHGDISABLE", w_string("1"));
  // This method is called from the eden watcher and can trigger before
  // mercurial has finalized writing out its history data.  Setting this
  // environmental variable allows us to break the view isolation and read
  // information about the commit before the transaction is complete.
  opt.environment().set("HG_PENDING", getRootPath());
  if (requestId && !requestId.empty()) {
    opt.environment().set("HGREQUESTID", requestId);
  }

  // Default to strict hg status.  HGDETECTRACE is used by some deployments
  // of mercurial to cause `hg status` to error out if it detects mutation
  // of the working copy that is happening currently with the status call.
  // This has to be opt-in behavior as it changes the semantics of the status
  // CLI invocation.  Watchman is ready to handle this case in a reasonably
  // defined manner, so we are safe to enable it.
  if (cfg_get_bool("fsmonitor.detectrace", true)) {
    opt.environment().set("HGDETECTRACE", w_string("1"));
  }

  opt.nullStdin();
  opt.pipeStdout();
  opt.pipeStderr();
  opt.chdir(getRootPath());

  return opt;
}

Mercurial::infoCache::infoCache(std::string path) : dirStatePath(path) {
  dirstate = FileInformation();
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

bool fileTimeEqual(const FileInformation& info1, const FileInformation& info2) {
  return (
      info1.size == info2.size && info1.mtime.tv_sec == info2.mtime.tv_sec &&
      info1.mtime.tv_nsec == info2.mtime.tv_nsec);
}

bool Mercurial::infoCache::dotChanged() {
  bool result;

  try {
    auto info = getFileInformation(dirStatePath.c_str(),
                                   CaseSensitivity::CaseSensitive);

    if (!fileTimeEqual(info, dirstate)) {
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

w_string Mercurial::mergeBaseWith(w_string_piece commitId, w_string requestId)
    const {
  std::string idString(commitId.data(), commitId.size());

  FileInformation startDirState;
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
    startDirState = cache->dirstate;
  }

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {hgExecutablePath(), "log", "-T", "{node}", "-r", revset},
      makeHgOptions(requestId));

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
    // Check that the dirState did not change, if it did it means that we raced
    // with another actor and that it is unsafe to blindly replace the cached
    // value with what we just computed, which we know would be stale
    // information for later consumers.  It is fine to continue using the data
    // we collected because we know that a pending change will advise clients of
    // the new state
    auto cache = cache_.wlock();
    if (fileTimeEqual(startDirState, cache->dirstate)) {
      cache->mergeBases[idString] = outputs.first;
    }
  }
  return outputs.first;
}

std::vector<w_string> Mercurial::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId,
    w_string requestId) const {
  // The "" argument at the end causes paths to be printed out relative to the
  // cwd (set to root path above).
  ChildProcess proc(
      {hgExecutablePath(), "status", "-n", "--rev", commitId, ""},
      makeHgOptions(requestId));

  auto outputs = proc.communicate();

  std::vector<w_string> lines;
  w_string_piece(outputs.first).split(lines, '\n');

  auto status = proc.wait();
  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the ",
        hgExecutablePath(),
        " status; command returned with status ",
        status,
        " out=",
        outputs.first,
        " err=",
        outputs.second));
  }

  return lines;
}

SCM::StatusResult Mercurial::getFilesChangedBetweenCommits(
    w_string_piece commitA,
    w_string_piece commitB,
    w_string requestId) const {
  // The "" argument at the end causes paths to be printed out
  // relative to the cwd (set to root path above).
  ChildProcess proc(
      {hgExecutablePath(),
       "status",
       "--print0",
       "--rev",
       commitA,
       "--rev",
       commitB,
       ""},
      makeHgOptions(requestId));

  auto outputs = proc.communicate();

  std::vector<w_string> lines;
  w_string_piece(outputs.first).split(lines, '\0');

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

  SCM::StatusResult result;
  log(DBG, "processing ", lines.size(), " status lines\n");
  for (auto& line : lines) {
    if (line.size() < 3) {
      continue;
    }
    w_string fileName(line.data() + 2, line.size() - 2);
    switch (line.data()[0]) {
      case 'A':
        result.addedFiles.emplace_back(std::move(fileName));
        break;
      case 'D':
        result.removedFiles.emplace_back(std::move(fileName));
        break;
      default:
        result.changedFiles.emplace_back(std::move(fileName));
    }
  }

  return result;
}

time_point<system_clock> Mercurial::getCommitDate(
    w_string_piece commitId,
    w_string requestId) const {
  ChildProcess proc(
      {hgExecutablePath(), "log", "-r", commitId.data(), "-T", "{date}\n"},
      makeHgOptions(requestId));
  auto outputs = proc.communicate();
  auto status = proc.wait();
  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for hg log; command returned with status ",
        status,
        " out=",
        outputs.first,
        " err=",
        outputs.second));
  }
  return Mercurial::convertCommitDate(outputs.first.c_str());
}

time_point<system_clock> Mercurial::convertCommitDate(const char* commitDate) {
  double date;
  if (std::sscanf(commitDate, "%lf", &date) != 1) {
    throw std::runtime_error(to<std::string>(
        "failed to parse date value `", commitDate, "` into a double"));
  }
  return system_clock::from_time_t(date);
}
} // namespace watchman
