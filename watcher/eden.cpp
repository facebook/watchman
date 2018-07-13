/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/String.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <algorithm>
#include "eden/fs/service/gen-cpp2/StreamingEdenService.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include "watchman.h"

#include "ChildProcess.h"
#include "LRUCache.h"
#include "QueryableView.h"
#include "ThreadPool.h"

using facebook::eden::EdenError;
using facebook::eden::FileDelta;
using facebook::eden::FileInformation;
using facebook::eden::FileInformationOrError;
using facebook::eden::Glob;
using facebook::eden::GlobParams;
using facebook::eden::JournalPosition;
using facebook::eden::SHA1Result;
using facebook::eden::StreamingEdenServiceAsyncClient;

namespace {
/** Represents a cache key for getFilesChangedBetweenCommits()
 * It is unfortunately a bit boilerplate-y due to the requirements
 * of unordered_map<>. */
struct BetweenCommitKey {
  std::string sinceCommit;
  std::string toCommit;

  bool operator==(const BetweenCommitKey& other) const {
    return sinceCommit == other.sinceCommit && toCommit == other.toCommit;
  }

  std::size_t hashValue() const {
    using namespace watchman;
    return hash_128_to_64(
        w_hash_bytes(sinceCommit.data(), sinceCommit.size(), 0),
        w_hash_bytes(toCommit.data(), toCommit.size(), 0));
  }
};
} // namespace

namespace std {
/** Ugly glue for unordered_map to hash BetweenCommitKey items */
template <>
struct hash<BetweenCommitKey> {
  std::size_t operator()(BetweenCommitKey const& key) const {
    return key.hashValue();
  }
};
} // namespace std

namespace watchman {
namespace {

/** This is a helper for settling out subscription events.
 * We have a single instance of the callback object that we schedule
 * each time we get an update from the eden server.  If we are already
 * scheduled we will cancel it and reschedule it.
 */
class SettleCallback : public folly::HHWheelTimer::Callback {
 public:
  SettleCallback(folly::EventBase* eventBase, std::shared_ptr<w_root_t> root)
      : eventBase_(eventBase), root_(std::move(root)) {}

  void timeoutExpired() noexcept override {
    try {
      auto settledPayload = json_object({{"settled", json_true()}});
      root_->unilateralResponses->enqueue(std::move(settledPayload));
    } catch (const std::exception& exc) {
      watchman::log(
          watchman::ERR,
          "error while dispatching settle payload; cancel watch: ",
          exc.what(),
          "\n");
      eventBase_->terminateLoopSoon();
    }
  }

  void callbackCanceled() noexcept override {
    // We must override this because the default is to call timeoutExpired().
    // We don't want that to happen because we're only canceled in the case
    // where we want to delay the timeoutExpired() callback.
  }

 private:
  folly::EventBase* eventBase_;
  std::shared_ptr<w_root_t> root_;
};

std::string readLink(const std::string& path) {
  struct stat st;
  if (lstat(path.c_str(), &st)) {
    throw std::system_error(
        errno,
        std::generic_category(),
        watchman::to<std::string>("lstat(", path, ") failed"));
  }
  std::string result(st.st_size + 1, '\0');
  auto len = ::readlink(path.c_str(), &result[0], result.size());
  if (len >= 0) {
    result.resize(len);
    return result;
  }

  throw std::system_error(
      errno,
      std::generic_category(),
      watchman::to<std::string>("readlink(", path, ") failed"));
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getEdenClient(
    w_string_piece rootPath,
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  // Resolve the eden socket; we use the .eden dir that is present in
  // every dir of an eden mount.
  folly::SocketAddress addr;
  auto path = watchman::to<std::string>(rootPath, "/.eden/socket");

  // It is important to resolve the link because the path in the eden mount
  // may exceed the maximum permitted unix domain socket path length.
  // This is actually how things our in our integration test environment.
  auto socketPath = readLink(path);
  addr.setFromPath(socketPath);

  return std::make_unique<StreamingEdenServiceAsyncClient>(
      apache::thrift::HeaderClientChannel::newChannel(
          apache::thrift::async::TAsyncSocket::newSocket(eb, addr)));
}

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(
      const w_string& rootPath,
      const FileInformationOrError& infoOrErr,
      const w_string& fullName,
      const SHA1Result* sha1 = nullptr,
      JournalPosition* position = nullptr,
      bool isNew = false)
      : root_path_(rootPath), fullName_(fullName) {
    otime_.ticks = ctime_.ticks = 0;
    otime_.timestamp = ctime_.timestamp = 0;
    if (position) {
      otime_.ticks = position->sequenceNumber;
      if (isNew) {
        // the "ctime" in the context of FileResult represents the point
        // in time that we saw the file transition !exists -> exists.
        // We don't strictly know the point at which that happened for results
        // returned from eden, but it will tell us whether that happened in
        // a given since query window by listing the file in the created files
        // set.  We set the isNew flag in this case.  The goal here is to
        // ensure that the code in query/eval.cpp considers us to be new too,
        // and that works because we set the created time ticks == the last
        // change tick.  The logic in query/eval.cpp will consider this to
        // be new because the ctime > lower bound in the since query.
        // When isNew is not set our ctime tick value is initialized to
        // zero which always fails that is_new check.
        ctime_.ticks = otime_.ticks;
      }
    }

    if (infoOrErr.getType() == FileInformationOrError::Type::info) {
      stat_.size = infoOrErr.get_info().size;
      stat_.mode = infoOrErr.get_info().mode;
      stat_.mtime.tv_sec = infoOrErr.get_info().mtime.seconds;
      stat_.mtime.tv_nsec = infoOrErr.get_info().mtime.nanoSeconds;

      otime_.timestamp = ctime_.timestamp = stat_.mtime.tv_sec;

      exists_ = true;

      if (sha1) {
        sha1_ = *sha1;
      }
    } else {
      exists_ = false;
      stat_ = watchman::FileInformation();
    }
  }

  const watchman::FileInformation& stat() override {
    return stat_;
  }

  w_string_piece baseName() override {
    return fullName_.piece().baseName();
  }

  w_string_piece dirName() override {
    return fullName_.piece().dirName();
  }

  bool exists() override {
    return exists_;
  }

  Future<w_string> readLink() override {
    return makeFuture<w_string>(nullptr);
  }

  const w_clock_t& ctime() override {
    return ctime_;
  }
  const w_clock_t& otime() override {
    return otime_;
  }

  Future<FileResult::ContentHash> getContentSha1() override {
    switch (sha1_.getType()) {
      // Copy thrift SHA1Result aka (std::string) into
      // watchman FileResult::ContentHash aka (std::array<uint8_t, 20>)
      case SHA1Result::Type::sha1: {
        auto& hash = sha1_.get_sha1();
        ContentHash hashData;
        std::copy(hash.begin(), hash.end(), hashData.begin());

        return makeFuture<FileResult::ContentHash>(makeResult(hashData));
      }

      // Thrift error occured
      case SHA1Result::Type::error:
        return makeFuture<FileResult::ContentHash>(
            Result<FileResult::ContentHash>(std::make_exception_ptr(
                std::runtime_error(sha1_.get_error().message))));

      // Something is wrong with type union
      default:
        return makeFuture<FileResult::ContentHash>(
            Result<FileResult::ContentHash>(std::make_exception_ptr(
                std::runtime_error("Unknown thrift data"))));
    }
  }

 private:
  w_string root_path_;
  w_string fullName_;
  watchman::FileInformation stat_;
  bool exists_;
  w_clock_t ctime_;
  w_clock_t otime_;
  SHA1Result sha1_;
};

static std::string escapeGlobSpecialChars(w_string_piece str) {
  std::string result;

  for (size_t i = 0; i < str.size(); ++i) {
    auto c = str[i];
    switch (c) {
      case '*':
      case '?':
      case '[':
      case ']':
      case '\\':
        result.append("\\");
        break;
    }
    result.append(&c, 1);
  }

  return result;
}

/** filter out paths that are ignored or that are not part of the
 * relative_root restriction in a query.
 * Ideally we'd pass this information into eden so that it doesn't
 * have to walk those paths and return the data to us, but for the
 * moment we have to filter it out of the results.
 * We need to respect the ignore_dirs configuration setting and
 * also remove anything that doesn't match the relative_root constraint
 * in the query.  The InMemoryView uses w_query_file_matches_relative_root()
 * for that purpose, but it cannot be re-used here because it operates
 * on the InMemoryView specific file representation that we can't recreate
 * here because of fundamental differences between the two watchers, and
 * also because we want to avoid materializing more information about the
 * file if we're just going to filter it out anyway.
 */
void filterOutPaths(std::vector<std::string>& fileNames, w_query_ctx* ctx) {
  fileNames.erase(
      std::remove_if(
          fileNames.begin(),
          fileNames.end(),
          [ctx](const std::string& name) {
            auto full = w_string::pathCat({ctx->root->root_path, name});

            if (ctx->query->relative_root) {
              auto parentPath = w_string_piece(full).dirName();

              if (!(parentPath == ctx->query->relative_root ||
                    parentPath.startsWith(ctx->query->relative_root_slash))) {
                // Not in the desired area, so filter it out
                return true;
              }
            }

            return ctx->root->ignore.isIgnored(full.data(), full.size());
          }),
      fileNames.end());
}

/** Wraps around the raw SCM to acclerate certain things for Eden
 */
class EdenWrappedSCM : public SCM {
  std::unique_ptr<SCM> inner_;
  std::string mountPoint_;

 public:
  explicit EdenWrappedSCM(std::unique_ptr<SCM> inner)
      : SCM(inner->getRootPath(), inner->getSCMRoot()),
        inner_(std::move(inner)),
        mountPoint_(to<std::string>(getRootPath())) {}

  w_string mergeBaseWith(w_string_piece commitId, w_string requestId = nullptr)
      const override {
    return inner_->mergeBaseWith(commitId, requestId);
  }
  std::vector<w_string> getFilesChangedSinceMergeBaseWith(
      w_string_piece commitId,
      w_string requestId = nullptr) const override {
    return inner_->getFilesChangedSinceMergeBaseWith(commitId, requestId);
  }

  SCM::StatusResult getFilesChangedBetweenCommits(
      w_string_piece commitA,
      w_string_piece commitB,
      w_string /* requestId */ = nullptr) const override {
    auto hashA = to<std::string>(commitA);
    auto hashB = to<std::string>(commitB);

    auto edenFuture =
        makeFuture()
            .via(&getThreadPool())
            .then([this, hashA, hashB](Result<Unit>) {
              return getFilesChangedBetweenCommitsFromEden(hashA, hashB);
            });

    auto hgFuture =
        makeFuture()
            .via(&getThreadPool())
            .then([this, hashA, hashB](Result<Unit>) {
              return inner_->getFilesChangedBetweenCommits(hashA, hashB);
            });

    return selectWinner(std::move(edenFuture), std::move(hgFuture)).get();
  }

  SCM::StatusResult getFilesChangedBetweenCommitsFromEden(
      const std::string& commitA,
      const std::string& commitB) const {
    using facebook::eden::BinaryHash;
    using facebook::eden::ScmFileStatus;
    using facebook::eden::ScmStatus;
    auto client = getEdenClient(getRootPath());
    ScmStatus status;
    client->sync_getScmStatusBetweenRevisions(
        status,
        mountPoint_,
        BinaryHash{to<std::string>(commitA)},
        BinaryHash{to<std::string>(commitB)});
    SCM::StatusResult result;
    for (const auto& it : status.entries) {
      w_string name(it.first.data(), it.first.size());
      switch (it.second) {
        case ScmFileStatus::ADDED:
          result.addedFiles.emplace_back(name);
          break;
        case ScmFileStatus::REMOVED:
          result.removedFiles.emplace_back(name);
          break;
        case ScmFileStatus::MODIFIED:
          result.changedFiles.emplace_back(name);
          break;
        case ScmFileStatus::IGNORED:
          /* impossible */
          break;
      }
    }
    return inner_->getFilesChangedBetweenCommits(commitA, commitB);
  }

  static std::unique_ptr<EdenWrappedSCM> wrap(std::unique_ptr<SCM> inner) {
    if (!inner) {
      return nullptr;
    }
    return std::make_unique<EdenWrappedSCM>(std::move(inner));
  }
};

/** Returns the files that match the glob. */
std::vector<std::string> callEdenGlobViaThrift(
    StreamingEdenServiceAsyncClient* client,
    const std::string& mountPoint,
    const std::vector<std::string>& globPatterns,
    bool includeDotfiles) {
  // Eden's Thrift API has an new Thrift API, globFiles(), and a deprecated
  // Thrift API, glob(). Because Thrift does not provide a way to dynamically
  // check which methods are available, we try globFiles() first and then fall
  // back to glob() if it fails. We should delete this code once we are
  // confident Eden instances in the wild have been updated to support
  // globFiles().
  try {
    GlobParams params;
    params.set_mountPoint(mountPoint);
    params.set_globs(globPatterns);
    params.set_includeDotfiles(includeDotfiles);

    // Turn on prefetching for file results.  Ideally we'd only do this
    // if we know that we need more than just the filenames, however,
    // since we unconditionally call getFileInformation() on each match
    // when producing the result objects, we'll always need the size
    // so unconditionally prefetching here should wind up being more
    // efficient.
    params.set_prefetchFiles(true);

    Glob glob;
    client->sync_globFiles(glob, params);
    return glob.get_matchingFiles();
  } catch (std::runtime_error& e) {
    // Note that the old API assumes includeDotfiles=true, so this will not
    // return the correct result if the caller specified includeDotfiles=false.
    if (!includeDotfiles) {
      throw std::runtime_error(
          "Run `eden restart` to pick up a new version of Eden "
          "so glob(includeDotfiles=false) is handled correctly.");
    }

    std::vector<std::string> fileNames;
    client->sync_glob(fileNames, mountPoint, globPatterns);
    return fileNames;
  }
}

class EdenView : public QueryableView {
  w_string root_path_;
  // The source control system that we detected during initialization
  mutable std::unique_ptr<EdenWrappedSCM> scm_;
  folly::EventBase subscriberEventBase_;
  mutable LRUCache<BetweenCommitKey, SCM::StatusResult>
      filesBetweenCommitCache_;
  JournalPosition lastCookiePosition_;
  std::string mountPoint_;

 public:
  explicit EdenView(w_root_t* root)
      : root_path_(root->root_path),
        scm_(EdenWrappedSCM::wrap(SCM::scmForPath(root->root_path))),
        // Allow for 32 pairs of revs, with errors cached for 10 seconds
        filesBetweenCommitCache_(32, std::chrono::seconds(10)),
        mountPoint_(to<std::string>(root->root_path)) {
    // Get the current journal position so that we can keep track of
    // cookie file changes
    auto client = getEdenClient(root_path_);
    client->sync_getCurrentJournalPosition(lastCookiePosition_, mountPoint_);
  }

  void timeGenerator(w_query* query, struct w_query_ctx* ctx) const override {
    auto client = getEdenClient(root_path_);

    FileDelta delta;
    JournalPosition resultPosition;

    if (ctx->since.is_timestamp) {
      throw QueryExecError(
          "timestamp based since queries are not supported with eden");
    }

    // This is the fall back for a fresh instance result set.
    // There are two different code paths that may need this, so
    // it is broken out as a lambda.
    auto getAllFiles = [this,
                        ctx,
                        &client,
                        includeDotfiles =
                            (query->glob_flags & WM_PERIOD) == 0]() {
      if (ctx->query->empty_on_fresh_instance) {
        // Avoid a full tree walk if we don't need it!
        return std::vector<std::string>();
      }

      std::string globPattern;
      if (ctx->query->relative_root) {
        w_string_piece rel(ctx->query->relative_root);
        rel.advance(ctx->root->root_path.size() + 1);
        globPattern.append(rel.data(), rel.size());
        globPattern.append("/");
      }
      globPattern.append("**");
      return callEdenGlobViaThrift(
          client.get(),
          mountPoint_,
          std::vector<std::string>{globPattern},
          includeDotfiles);
    };

    std::vector<std::string> fileNames;
    // We use the list of created files to synthesize the "new" field
    // in the file results
    std::unordered_set<std::string> createdFileNames;

    if (ctx->since.clock.is_fresh_instance) {
      // Earlier in the processing flow, we decided that the rootNumber
      // didn't match the current root which means that eden was restarted.
      // We need to translate this to a fresh instance result set and
      // return a list of all possible matching files.
      client->sync_getCurrentJournalPosition(resultPosition, mountPoint_);
      fileNames = getAllFiles();
    } else {
      // Query eden to fill in the mountGeneration field.
      JournalPosition position;
      client->sync_getCurrentJournalPosition(position, mountPoint_);
      // dial back to the sequence number from the query
      position.sequenceNumber = ctx->since.clock.ticks;

      // Now we can get the change journal from eden
      try {
        client->sync_getFilesChangedSince(delta, mountPoint_, position);

        createdFileNames.insert(
            delta.createdPaths.begin(), delta.createdPaths.end());

        // The list of changed files is the union of the created, added,
        // and removed sets returned from eden in list form.
        fileNames = std::move(delta.changedPaths);
        fileNames.insert(
            fileNames.end(),
            std::make_move_iterator(delta.removedPaths.begin()),
            std::make_move_iterator(delta.removedPaths.end()));
        fileNames.insert(
            fileNames.end(),
            std::make_move_iterator(delta.createdPaths.begin()),
            std::make_move_iterator(delta.createdPaths.end()));

        if (scm_ &&
            delta.fromPosition.snapshotHash != delta.toPosition.snapshotHash) {
          // Either they checked out a new commit or reset the commit to
          // a different hash.  We interrogate source control to discover
          // the set of changed files between those hashes, and then
          // add in any paths that may have changed around snapshot hash
          // changes events;  These are files whose status cannot be
          // determined purely from source control operations

          std::unordered_set<std::string> mergedFileList(
              fileNames.begin(), fileNames.end());

          auto fromHash = folly::hexlify(delta.fromPosition.snapshotHash);
          auto toHash = folly::hexlify(delta.toPosition.snapshotHash);
          log(ERR,
              "since ",
              position.sequenceNumber,
              " we changed commit hashes from ",
              fromHash,
              " to ",
              toHash,
              "\n");

          auto changedBetweenCommits =
              getFilesChangedBetweenCommits(fromHash, toHash);

          for (auto& fileName : changedBetweenCommits.changedFiles) {
            mergedFileList.insert(to<std::string>(fileName));
          }
          for (auto& fileName : changedBetweenCommits.removedFiles) {
            mergedFileList.insert(to<std::string>(fileName));
          }
          for (auto& fileName : changedBetweenCommits.addedFiles) {
            mergedFileList.insert(to<std::string>(fileName));
            createdFileNames.insert(to<std::string>(fileName));
          }

          // We don't know whether the unclean paths are added, removed
          // or just changed.  We're going to treat them as changed.
          mergedFileList.insert(
              std::make_move_iterator(delta.uncleanPaths.begin()),
              std::make_move_iterator(delta.uncleanPaths.end()));

          // Replace the list of fileNames with the de-duped set
          // of names we've extracted from source control
          fileNames.clear();
          fileNames.insert(
              fileNames.end(),
              std::make_move_iterator(mergedFileList.begin()),
              std::make_move_iterator(mergedFileList.end()));
        }

        resultPosition = delta.toPosition;
        watchman::log(
            watchman::DBG,
            "wanted from ",
            position.sequenceNumber,
            " result delta from ",
            delta.fromPosition.sequenceNumber,
            " to ",
            delta.toPosition.sequenceNumber,
            " with ",
            fileNames.size(),
            " changed files\n");
      } catch (const EdenError& err) {
        if (err.errorCode != ERANGE) {
          throw;
        }
        // mountGeneration differs, so treat this as equivalent
        // to a fresh instance result
        ctx->since.clock.is_fresh_instance = true;
        client->sync_getCurrentJournalPosition(resultPosition, mountPoint_);
        fileNames = getAllFiles();
      }
    }

    // Filter out any ignored files
    filterOutPaths(fileNames, ctx);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint_, fileNames);

    if (info.size() != fileNames.size()) {
      throw QueryExecError(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    // If the query requires content.sha1hex, fetch those in a batch now
    std::vector<SHA1Result> sha1s;
    auto sha1Requested = query->isFieldRequested("content.sha1hex");
    if (sha1Requested) {
      client->sync_getSHA1(sha1s, mountPoint_, fileNames);
      if (sha1s.size() != fileNames.size()) {
        throw QueryExecError(
            "sha1s.size() didn't match fileNames.size(), should be unpossible!");
      }
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    auto shaIter = sha1s.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;
      // a file is considered new if it was present in the created files
      // set returned from eden.
      bool isNew = createdFileNames.find(name) != createdFileNames.end();

      auto file = make_unique<EdenFileResult>(
          root_path_,
          fileInfo,
          w_string::pathCat({mountPoint_, name}),
          sha1Requested ? &*shaIter : nullptr,
          &resultPosition,
          isNew);

      w_query_process_file(ctx->query, ctx, std::move(file));

      ++nameIter;
      ++infoIter;
      ++shaIter;
    }

    ctx->bumpNumWalked(fileNames.size());
  }

  void suffixGenerator(w_query* query, struct w_query_ctx* ctx) const override {
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    w_string_piece rel;
    if (query->relative_root) {
      rel = query->relative_root;
      rel.advance(ctx->root->root_path.size() + 1);
    }

    std::vector<std::string> globStrings;
    // Translate the suffix list into a list of globs
    for (auto& suff : query->suffixes) {
      globStrings.emplace_back(to<std::string>(w_string::pathCat(
          {rel, to<std::string>("**/*.", escapeGlobSpecialChars(suff))})));
    }
    executeGlobBasedQuery(globStrings, query, ctx);
  }

  void syncToNow(const std::shared_ptr<w_root_t>&, std::chrono::milliseconds)
      override {}

  void executeGlobBasedQuery(
      const std::vector<std::string>& globStrings,
      w_query* query,
      struct w_query_ctx* ctx) const {
    auto client = getEdenClient(ctx->root->root_path);

    auto includeDotfiles = (query->glob_flags & WM_PERIOD) == 0;
    auto fileNames = callEdenGlobViaThrift(
        client.get(), mountPoint_, globStrings, includeDotfiles);

    // Filter out any ignored files
    filterOutPaths(fileNames, ctx);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint_, fileNames);

    if (info.size() != fileNames.size()) {
      throw QueryExecError(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    // If the query requires content.sha1hex, fetch those in a batch now
    std::vector<SHA1Result> sha1s;
    auto sha1Requested = query->isFieldRequested("content.sha1hex");
    if (sha1Requested) {
      client->sync_getSHA1(sha1s, mountPoint_, fileNames);
      if (sha1s.size() != fileNames.size()) {
        throw QueryExecError(
            "sha1s.size() didn't match fileNames.size(), should be unpossible!");
      }
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    auto shaIter = sha1s.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          root_path_,
          fileInfo,
          w_string::pathCat({mountPoint_, name}),
          sha1Requested ? &*shaIter : nullptr);

      w_query_process_file(ctx->query, ctx, std::move(file));

      ++nameIter;
      ++infoIter;
      ++shaIter;
    }

    ctx->bumpNumWalked(fileNames.size());
  }

  // Helper for computing a relative path prefix piece.
  // The returned piece is owned by the supplied context object!
  w_string_piece computeRelativePathPiece(struct w_query_ctx* ctx) const {
    w_string_piece rel;
    if (ctx->query->relative_root) {
      rel = ctx->query->relative_root;
      rel.advance(ctx->root->root_path.size() + 1);
    }
    return rel;
  }

  /** Walks files that match the supplied set of paths */
  void pathGenerator(w_query* query, struct w_query_ctx* ctx) const override {
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    // Translate the path list into a list of globs
    for (auto& path : query->paths) {
      if (path.depth > 0) {
        // We don't have an easy way to express depth constraints
        // in the existing glob API, so we just punt for the moment.
        // I believe that this sort of query is quite rare anyway.
        throw QueryExecError(
            "the eden watcher only supports depth 0 or depth -1");
      }
      // -1 depth is infinite which we can translate to a recursive
      // glob.  0 depth is direct descendant which we can translate
      // to a simple * wildcard.
      auto glob = path.depth == -1 ? "**/*" : "*";

      globStrings.emplace_back(to<std::string>(
          w_string::pathCat({rel, escapeGlobSpecialChars(path.name), glob})));
    }
    executeGlobBasedQuery(globStrings, query, ctx);
  }

  void globGenerator(w_query* query, struct w_query_ctx* ctx) const override {
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    // Use the glob array provided by the query_spec.
    // The InMemoryView uses the compiled glob tree but we just want to
    // pass this list through to eden to evaluate.  Note that we're
    // relying on parse_globs() to have already checked that the glob
    // looks sane during query parsing, and that eden itself will
    // sanity check and throw an error if there is still something it
    // doesn't like about it when we call sync_glob() below.
    for (auto& glob : query->query_spec.get("glob").array()) {
      globStrings.emplace_back(
          to<std::string>(w_string::pathCat({rel, json_to_w_string(glob)})));
    }

    // More glob flags/functionality:
    auto noescape = json_is_true(
        query->query_spec.get_default("glob_noescape", json_false()));
    if (noescape) {
      throw QueryExecError(
          "glob_noescape is not supported for the eden watcher");
    }
    executeGlobBasedQuery(globStrings, query, ctx);
  }

  void allFilesGenerator(w_query* query, struct w_query_ctx* ctx)
      const override {
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    std::string globPattern;
    auto rel = computeRelativePathPiece(ctx);
    if (rel.size() > 0) {
      globPattern.append(rel.data(), rel.size());
      globPattern.append("/");
    }
    globPattern.append("**");
    executeGlobBasedQuery(std::vector<std::string>{globPattern}, query, ctx);
  }

  ClockPosition getMostRecentRootNumberAndTickValue() const override {
    auto client = getEdenClient(root_path_);
    JournalPosition position;
    client->sync_getCurrentJournalPosition(position, mountPoint_);
    return ClockPosition(position.mountGeneration, position.sequenceNumber);
  }

  w_string getCurrentClockString() const override {
    return getMostRecentRootNumberAndTickValue().toClockString();
  }

  uint32_t getLastAgeOutTickValue() const override {
    return 0;
  }

  time_t getLastAgeOutTimeStamp() const override {
    return 0;
  }

  void ageOut(w_perf_t& /*sample*/, std::chrono::seconds /*minAge*/) override {}

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& /*fileNames*/) const override {
    return false;
  }

  SCM* getSCM() const override {
    return scm_.get();
  }

  SCM::StatusResult getFilesChangedBetweenCommits(
      w_string_piece commitA,
      w_string_piece commitB) const {
    BetweenCommitKey key{to<std::string>(commitA), to<std::string>(commitB)};
    auto result =
        filesBetweenCommitCache_
            .get(
                key,
                [this](const BetweenCommitKey& cacheKey) {
                  return makeFuture(getSCM()->getFilesChangedBetweenCommits(
                      cacheKey.sinceCommit, cacheKey.toCommit));
                })
            .get();
    return result->value();
  }

  void startThreads(const std::shared_ptr<w_root_t>& root) override {
    auto self = shared_from_this();
    std::thread thr([self, this, root]() { subscriberThread(root); });
    thr.detach();
  }

  void signalThreads() override {
    subscriberEventBase_.terminateLoopSoon();
  }

  // Called by the subscriberThread to scan for cookie file creation
  // events.  These are used to manage sequencing for state-enter and
  // state-leave in eden.
  void checkCookies(const std::shared_ptr<w_root_t>& root) {
    // Obtain the list of changes since our last request, or since we started
    // up the watcher (we set the initial value of lastCookiePosition_ during
    // construction).
    FileDelta delta;
    auto client = getEdenClient(root_path_);
    client->sync_getFilesChangedSince(delta, mountPoint_, lastCookiePosition_);

    // TODO: in the future it would be nice to compute the paths in a loop
    // first, and then add a bulk CookieSync::notifyCookies() method to avoid
    // locking and unlocking its internal mutex so frequently.
    for (auto& file : delta.createdPaths) {
      auto full = w_string::pathCat({root_path_, file});
      root->cookies.notifyCookie(full);
    }

    // Remember this position for subsequent calls
    lastCookiePosition_ = delta.toPosition;
  }

  // This is the thread that we use to listen to the stream of
  // changes coming in from the Eden server
  void subscriberThread(std::shared_ptr<w_root_t> root) noexcept {
    SCOPE_EXIT {
      // ensure that the root gets torn down,
      // otherwise we'd leave it in a broken state.
      root->cancel();
    };

    w_set_thread_name("edensub %s", root->root_path.c_str());
    watchman::log(watchman::DBG, "Started subscription thread\n");

    try {
      // Prepare the callback
      SettleCallback settleCallback(&subscriberEventBase_, root);
      // Figure out the correct value for settling
      std::chrono::milliseconds settleTimeout(root->trigger_settle);

      // Connect up the client
      auto client = getEdenClient(root->root_path, &subscriberEventBase_);

      // This is called each time we get pushed an update by the eden server
      auto onUpdate = [&](apache::thrift::ClientReceiveState&& state) mutable {
        if (!state.isStreamEnd()) {
          try {
            JournalPosition pos;
            StreamingEdenServiceAsyncClient::recv_subscribe(pos, state);

            if (settleCallback.isScheduled()) {
              watchman::log(watchman::DBG, "reschedule settle timeout\n");
              settleCallback.cancelTimeout();
            }
            subscriberEventBase_.timer().scheduleTimeout(
                &settleCallback, settleTimeout);

            // We need to process cookie files with the lowest possible
            // latency, so we consume that information now
            checkCookies(root);

          } catch (const std::exception& exc) {
            watchman::log(
                watchman::ERR,
                "error while receiving subscription; cancel watch: ",
                exc.what(),
                "\n");
            // make sure we don't get called again
            subscriberEventBase_.terminateLoopSoon();
            return;
          }
        }

        if (state.isStreamEnd()) {
          watchman::log(
              watchman::ERR, "subscription stream ended, cancel watch\n");
          // We won't be called again, but we terminate the loop just
          // to make sure.
          subscriberEventBase_.terminateLoopSoon();
          return;
        }
      };

      // Establish the subscription stream
      client->subscribe(
          onUpdate,
          std::string(root->root_path.data(), root->root_path.size()));

      // This will run until the stream ends
      subscriberEventBase_.loop();

    } catch (const std::exception& exc) {
      watchman::log(
          watchman::ERR,
          "uncaught exception in subscription thread, cancel watch:",
          exc.what(),
          "\n");
    }
  }

  const w_string& getName() const override {
    static w_string name("eden");
    return name;
  }

  std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<w_root_t>& /*root*/) override {
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }
};

std::shared_ptr<watchman::QueryableView> detectEden(w_root_t* root) {
  auto edenRoot =
      readLink(watchman::to<std::string>(root->root_path, "/.eden/root"));
  if (w_string_piece(edenRoot) != root->root_path) {
    // We aren't at the root of the eden mount
    throw TerminalWatcherError(to<std::string>(
        "you may only watch from the root of an eden mount point. "
        "Try again using ",
        edenRoot));
  }

  try {
    auto client = getEdenClient(root->root_path);

    // We don't strictly need to do this, since we just verified that the root
    // matches our expectations, but it can't hurt to attempt to talk to the
    // daemon directly, just in case it is broken for some reason, or in
    // case someone is trolling us with a directory structure that looks
    // like an eden mount.
    std::vector<FileInformationOrError> info;
    static const std::vector<std::string> paths{""};
    client->sync_getFileInformation(
        info,
        std::string(root->root_path.data(), root->root_path.size()),
        paths);

    return std::make_shared<EdenView>(root);
  } catch (const std::exception& exc) {
    throw TerminalWatcherError(to<std::string>(
        "failed to communicate with eden mount ", edenRoot, ": ", exc.what()));
  }
}

} // namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // namespace watchman
