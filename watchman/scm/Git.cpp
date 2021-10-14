/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/scm/Git.h"
#include <folly/String.h>
#include "watchman/ChildProcess.h"
#include "watchman/CommandRegistry.h"
#include "watchman/Logging.h"
#include "watchman/fs/FileSystem.h"

// Capability indicating support for the git SCM
W_CAP_REG("scm-git")

using namespace std::chrono;
using folly::to;

namespace {
using namespace watchman;

void replaceEmbeddedNulls(std::string& str) {
  std::replace(str.begin(), str.end(), '\0', '\n');
}

std::string gitExecutablePath() {
  return "git";
}

struct GitResult {
  w_string output;
};

GitResult runGit(
    std::vector<std::string_view> cmdline,
    ChildProcess::Options options,
    std::string_view description) {
  ChildProcess proc{cmdline, std::move(options)};
  auto outputs = proc.communicate();
  auto status = proc.wait();
  if (status) {
    auto output = std::string{outputs.first.view()};
    auto error = std::string{outputs.second.view()};
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
        error,
        "\nstatus = ",
        to<std::string>(status)};
  }

  return GitResult{std::move(outputs.first)};
}

} // namespace

namespace watchman {

void GitStatusAccumulator::add(w_string_piece status) {
  std::vector<w_string_piece> lines;
  status.split(lines, '\0');

  log(DBG, "processing ", lines.size(), " status lines\n");

  for (auto& line : lines) {
    if (line.size() < 4) {
      continue;
    }

    w_string name{line.data() + 3, line.size() - 3};
    switch (line.data()[1]) {
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

SCM::StatusResult GitStatusAccumulator::finalize() const {
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

Git::Git(w_string_piece rootPath, w_string_piece scmRoot)
    : SCM(rootPath, scmRoot),
      indexPath_(to<std::string>(getSCMRoot().view(), "/.git/index")),
      commitsPrior_(Configuration(), "scm_git_commits_prior", 32, 10),
      mergeBases_(Configuration(), "scm_git_mergebase", 32, 10),
      filesChangedBetweenCommits_(
          Configuration(),
          "scm_git_files_between_commits",
          32,
          10),
      filesChangedSinceMergeBaseWith_(
          Configuration(),
          "scm_git_files_since_mergebase",
          32,
          10) {}

ChildProcess::Options Git::makeGitOptions(w_string requestId) const {
  ChildProcess::Options opt;
  (void)requestId;
  opt.nullStdin();
  opt.pipeStdout();
  opt.pipeStderr();
  opt.chdir(getRootPath());
  return opt;
}

struct timespec Git::getIndexMtime() const {
  try {
    auto info =
        getFileInformation(indexPath_.c_str(), CaseSensitivity::CaseSensitive);
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

w_string Git::mergeBaseWith(w_string_piece commitId, w_string requestId) const {
  auto mtime = getIndexMtime();
  auto key = folly::to<std::string>(
      commitId.view(), ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commit = std::string{commitId.view()};

  return mergeBases_
      .get(
          key,
          [this, commit, requestId](const std::string&) {
            auto result = runGit(
                {gitExecutablePath(), "merge-base", commit, "HEAD"},
                makeGitOptions(requestId),
                "query for the merge base");

            auto output = std::string{result.output.view()};
            if (!output.empty() && output.back() == '\n') {
              output.pop_back();
            }

            if (output.size() != 40) {
              throw SCMError(
                  "expected merge base to be a 40 character string, got ",
                  output);
            }

            // TODO: is w_string(s.c_str()) safe?
            return folly::makeFuture(w_string(output.c_str()));
          })
      .get()
      ->value();
}

std::vector<w_string> Git::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId,
    w_string requestId) const {
  auto mtime = getIndexMtime();
  auto key = folly::to<std::string>(
      commitId.view(), ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commitCopy = std::string{commitId.view()};
  return filesChangedSinceMergeBaseWith_
      .get(
          key,
          [this, commit = std::move(commitCopy), requestId](
              const std::string&) {
            auto result = runGit(
                {gitExecutablePath(), "diff", "--name-only", "-z", commit},
                makeGitOptions(requestId),
                "query for files changed since merge base");

            std::vector<w_string> lines;
            w_string_piece(result.output).split(lines, '\0');
            return folly::makeFuture(lines);
          })
      .get()
      ->value();
}

SCM::StatusResult Git::getFilesChangedBetweenCommits(
    std::vector<std::string> commits,
    w_string requestId) const {
  GitStatusAccumulator result;
  for (size_t i = 0; i + 1 < commits.size(); ++i) {
    auto mtime = getIndexMtime();
    auto& commitA = commits[i];
    auto& commitB = commits[i + 1];
    auto key = folly::to<std::string>(
        commitA, ":", commitB, ":", mtime.tv_sec, ":", mtime.tv_nsec);

    result.add(filesChangedBetweenCommits_
                   .get(
                       key,
                       [&](const std::string&) {
                         auto gitresult = runGit(
                             {gitExecutablePath(),
                              "diff",
                              "--name-status",
                              "-z",
                              commitA,
                              commitB},
                             makeGitOptions(requestId),
                             "get files changed between commits");

                         return folly::makeFuture(gitresult.output);
                       })
                   .get()
                   ->value());
  }
  return result.finalize();
}

std::chrono::time_point<std::chrono::system_clock> Git::getCommitDate(
    w_string_piece commitId,
    w_string requestId) const {
  auto result = runGit(
      {gitExecutablePath(), "log", "--format:%ct", "-n", "1", commitId.view()},
      makeGitOptions(requestId),
      "get commit date");
  double timestamp;
  if (std::sscanf(result.output.c_str(), "%lf", &timestamp) != 1) {
    throw std::runtime_error(to<std::string>(
        "failed to parse date value `",
        result.output.view(),
        "` into a double"));
  }
  return system_clock::from_time_t(timestamp);
}

std::vector<w_string> Git::getCommitsPriorToAndIncluding(
    w_string_piece commitId,
    int numCommits,
    w_string requestId) const {
  auto mtime = getIndexMtime();
  auto key = folly::to<std::string>(
      commitId.view(), ":", numCommits, ":", mtime.tv_sec, ":", mtime.tv_nsec);
  auto commitCopy = std::string{commitId.view()};

  return commitsPrior_
      .get(
          key,
          [this, commit = std::move(commitCopy), numCommits, requestId](
              const std::string&) {
            auto result = runGit(
                {gitExecutablePath(),
                 "log",
                 "-n",
                 to<std::string>(numCommits),
                 "--format=%H",
                 commit},
                makeGitOptions(requestId),
                "get prior commits");

            std::vector<w_string> lines;
            w_string_piece(result.output).split(lines, '\n');
            return folly::makeFuture(lines);
          })
      .get()
      ->value();
}

} // namespace watchman
