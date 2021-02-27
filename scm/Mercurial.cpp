/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman.h"
#include "Mercurial.h"
#include <folly/String.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "ChildProcess.h"
#include "Logging.h"

// Capability indicating support for the mercurial SCM
W_CAP_REG("scm-hg")

using namespace std::chrono;
using folly::to;

namespace {
using namespace watchman;

void replaceEmbeddedNulls(std::string& str) {
  std::replace(str.begin(), str.end(), '\0', '\n');
}

std::string hgExecutablePath() {
  auto hg = getenv("EDEN_HG_BINARY");
  if (hg && strlen(hg) > 0) {
    return hg;
  }
  return "hg";
}

struct MercurialResult {
  w_string output;
};

MercurialResult runMercurial(
    std::vector<w_string_piece> cmdline,
    ChildProcess::Options options,
    folly::StringPiece description) {
  ChildProcess proc{cmdline, std::move(options)};
  auto outputs = proc.communicate();
  auto status = proc.wait();
  if (status) {
    auto output = folly::StringPiece{outputs.first}.str();
    auto error = folly::StringPiece{outputs.second}.str();
    replaceEmbeddedNulls(output);
    replaceEmbeddedNulls(error);
    throw SCMError{
        "failed to ",
        description,
        "\ncmd = ",
        folly::join(" ", cmdline),
        "\nstdout = ",
        output,
        "\nstderr = ",
        error};
  }

  return MercurialResult{std::move(outputs.first)};
}

} // namespace

namespace watchman {

void StatusAccumulator::add(w_string_piece status) {
  std::vector<w_string_piece> lines;
  status.split(lines, '\0');

  log(DBG, "processing ", lines.size(), " status lines\n");

  for (auto& line : lines) {
    if (line.size() < 3) {
      continue;
    }

    w_string name{line.data() + 2, line.size() - 2};
    switch (line.data()[0]) {
      case 'A':
        // Should remove + add be considered new? Treat it as changed for now.
        byFile_[name] += 1;
        break;
      case 'D':
        byFile_[name] += -1;
        break;
      default:
        byFile_[name]; // just insert an entry
    }
  }
}

SCM::StatusResult StatusAccumulator::finalize() const {
  SCM::StatusResult combined;
  for (auto& [name, count] : byFile_) {
    if (count == 0) {
      combined.changedFiles.push_back(name);
    } else if (count < 0) {
      combined.removedFiles.push_back(name);
    } else if (count > 0) {
      combined.addedFiles.push_back(name);
    }
  }
  return combined;
}

ChildProcess::Options Mercurial::makeHgOptions(w_string requestId) const {
  ChildProcess::Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", w_string("1"));
  // Ensure that we do not telemetry log profiling data for the commands we are
  // running by default. This is to avoid a significant increase in the rate of
  // logging.
  if (!cfg_get_bool("enable_hg_telemetry_logging", false)) {
    opt.environment().set("NOSCMLOG", w_string("1"));
  }
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

  // Ensure that mercurial uses this path to communicate with us,
  // rather than whatever is hardcoded in its config.
  opt.environment().set("WATCHMAN_SOCK", get_sock_name_legacy());

  opt.nullStdin();
  opt.pipeStdout();
  opt.pipeStderr();
  opt.chdir(getRootPath());

  return opt;
}

Mercurial::Mercurial(w_string_piece rootPath, w_string_piece scmRoot)
    : SCM(rootPath, scmRoot),
      dirStatePath_(to<std::string>(getSCMRoot(), "/.hg/dirstate")),
      commitsPrior_(Configuration(), "scm_hg_commits_prior", 32, 10),
      mergeBases_(Configuration(), "scm_hg_mergebase", 32, 10),
      filesChangedBetweenCommits_(
          Configuration(),
          "scm_hg_files_between_commits",
          32,
          10),
      filesChangedSinceMergeBaseWith_(
          Configuration(),
          "scm_hg_files_since_mergebase",
          32,
          10) {}

struct timespec Mercurial::getDirStateMtime() const {
  try {
    auto info = getFileInformation(
        dirStatePath_.c_str(), CaseSensitivity::CaseSensitive);
    return info.mtime;
  } catch (const std::system_error&) {
    // Failed to stat, so assume the current time
    struct timeval now;
    gettimeofday(&now, nullptr);
    struct timespec ts;
    ts.tv_sec = now.tv_sec;
    ts.tv_nsec = now.tv_usec * 1000;
    return ts;
  }
}

w_string Mercurial::mergeBaseWith(w_string_piece commitId, w_string requestId)
    const {
  auto mtime = getDirStateMtime();
  auto key =
      folly::to<std::string>(commitId, ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commit = folly::to<std::string>(commitId);

  return mergeBases_
      .get(
          key,
          [this, commit, requestId](const std::string&) {
            auto revset = to<std::string>("ancestor(.,", commit, ")");
            auto result = runMercurial(
                {hgExecutablePath(), "log", "-T", "{node}", "-r", revset},
                makeHgOptions(requestId),
                "query for the merge base");

            if (result.output.size() != 40) {
              throw SCMError(
                  "expected merge base to be a 40 character string, got ",
                  result.output);
            }

            return folly::makeFuture(result.output);
          })
      .get()
      ->value();
}

std::vector<w_string> Mercurial::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId,
    w_string requestId) const {
  auto mtime = getDirStateMtime();
  auto key =
      folly::to<std::string>(commitId, ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commitCopy = folly::to<std::string>(commitId);

  return filesChangedSinceMergeBaseWith_
      .get(
          key,
          [this, commit = std::move(commitCopy), requestId](
              const std::string&) {
            auto result = runMercurial(
                {hgExecutablePath(),
                 "--traceback",
                 "status",
                 "-n",
                 "--rev",
                 commit,
                 // The "" argument at the end causes paths to be printed out
                 // relative to the cwd (set to root path above).
                 ""},
                makeHgOptions(requestId),
                "query for files changed since merge base");

            std::vector<w_string> lines;
            w_string_piece(result.output).split(lines, '\n');
            return folly::makeFuture(lines);
          })
      .get()
      ->value();
}

SCM::StatusResult Mercurial::getFilesChangedBetweenCommits(
    std::vector<std::string> commits,
    w_string requestId) const {
  StatusAccumulator result;
  for (size_t i = 0; i + 1 < commits.size(); ++i) {
    auto mtime = getDirStateMtime();
    auto& commitA = commits[i];
    auto& commitB = commits[i + 1];
    if (commitA == commitB) {
      // Older versions of EdenFS could report "commit transitions" from A to A,
      // in which case we shouldn't ask Mercurial for the difference.
      continue;
    }
    auto key = folly::to<std::string>(
        commitA, ":", commitB, ":", mtime.tv_sec, ":", mtime.tv_nsec);

    // This loop runs `hg status` commands sequentially. There's an opportunity
    // to run them concurrently, but:
    // 1. In practice since each transition in `commits` corresponds to an
    //    `hg update` call, the list is almost always short.
    // 2. For debugging Watchman performance issues, it's nice to have the
    //    subprocess call on the same stack.
    // 3. If `hg status` acquires a lock on the backing storage, there may not
    //    be much actual concurrency.
    // 4. This codepath is most frequently executed under very fast checkout
    //    operations between close commits, where the cost isn't that high.

    result.add(
        filesChangedBetweenCommits_
            .get(
                key,
                [&](const std::string&) {
                  auto hgresult = runMercurial(
                      {hgExecutablePath(),
                       "--traceback",
                       "status",
                       "--print0",
                       "--rev",
                       commitA,
                       "--rev",
                       commitB,
                       // The "" argument at the end causes paths to be printed
                       // out relative to the cwd (set to root path above).
                       ""},
                      makeHgOptions(requestId),
                      "get files changed between commits");

                  return folly::makeFuture(hgresult.output);
                })
            .get()
            ->value());
  }
  return result.finalize();
}

time_point<system_clock> Mercurial::getCommitDate(
    w_string_piece commitId,
    w_string requestId) const {
  auto result = runMercurial(
      {hgExecutablePath(),
       "--traceback",
       "log",
       "-r",
       commitId.data(),
       "-T",
       "{date}\n"},
      makeHgOptions(requestId),
      "get commit date");
  return Mercurial::convertCommitDate(result.output.c_str());
}

time_point<system_clock> Mercurial::convertCommitDate(const char* commitDate) {
  double date;
  if (std::sscanf(commitDate, "%lf", &date) != 1) {
    throw std::runtime_error(to<std::string>(
        "failed to parse date value `", commitDate, "` into a double"));
  }
  return system_clock::from_time_t(date);
}

std::vector<w_string> Mercurial::getCommitsPriorToAndIncluding(
    w_string_piece commitId,
    int numCommits,
    w_string requestId) const {
  auto mtime = getDirStateMtime();
  auto key = folly::to<std::string>(
      commitId, ":", numCommits, ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commitCopy = folly::to<std::string>(commitId);

  return commitsPrior_
      .get(
          key,
          [this, commit = std::move(commitCopy), numCommits, requestId](
              const std::string&) {
            auto revset = to<std::string>(
                "reverse(last(_firstancestors(",
                commit,
                "), ",
                numCommits,
                "))\n");
            auto result = runMercurial(
                {hgExecutablePath(),
                 "--traceback",
                 "log",
                 "-r",
                 revset,
                 "-T",
                 "{node}\n"},
                makeHgOptions(requestId),
                "get prior commits");

            std::vector<w_string> lines;
            w_string_piece(result.output).split(lines, '\n');
            return folly::makeFuture(lines);
          })
      .get()
      ->value();
}

} // namespace watchman
