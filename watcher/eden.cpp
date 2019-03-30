/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <folly/String.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/async/RSocketClientChannel.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include "eden/fs/service/gen-cpp2/StreamingEdenService.h"
#include "thirdparty/wildmatch/wildmatch.h"
#include "watchman_error_category.h"

#include "ChildProcess.h"
#include "LRUCache.h"
#include "QueryableView.h"
#include "ThreadPool.h"

using apache::thrift::async::TAsyncSocket;
using facebook::eden::EdenError;
using facebook::eden::FileDelta;
using facebook::eden::FileInformation;
using facebook::eden::FileInformationOrError;
using facebook::eden::Glob;
using facebook::eden::GlobParams;
using facebook::eden::JournalPosition;
using facebook::eden::SHA1Result;
using facebook::eden::StreamingEdenServiceAsyncClient;
using folly::Optional;
using std::make_unique;

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

/** Execute a functor, retrying it if we encounter an ESTALE exception.
 * Ideally ESTALE wouldn't happen but we've been unable to figure out
 * exactly what is happening on the Eden side so far, and it is more
 * expedient to add a basic retry to ensure smoother operation
 * for watchman clients. */
template <typename FUNC>
auto retryEStale(FUNC&& func) {
  constexpr size_t kNumRetries = 5;
  std::chrono::milliseconds backoff(1);
  for (size_t retryAttemptsRemaining = kNumRetries; retryAttemptsRemaining >= 0;
       --retryAttemptsRemaining) {
    try {
      return func();
    } catch (const std::system_error& exc) {
      if (exc.code() != error_code::stale_file_handle ||
          retryAttemptsRemaining == 0) {
        throw;
      }
      // Try again
      log(ERR,
          "Got ESTALE error from eden; will retry ",
          retryAttemptsRemaining,
          " more times. (",
          exc.what(),
          ")\n");
      /* sleep override */ std::this_thread::sleep_for(backoff);
      backoff *= 2;
      continue;
    }
  }
  throw std::runtime_error(
      "unreachable line reached; should have thrown an ESTALE");
}

folly::SocketAddress getEdenSocketAddress(w_string_piece rootPath) {
  // Resolve the eden socket; we use the .eden dir that is present in
  // every dir of an eden mount.
  folly::SocketAddress addr;
  auto path = watchman::to<std::string>(rootPath, "/.eden/socket");

  // It is important to resolve the link because the path in the eden mount
  // may exceed the maximum permitted unix domain socket path length.
  // This is actually how things our in our integration test environment.
  auto socketPath = readSymbolicLink(path.c_str());
  addr.setFromPath(watchman::to<std::string>(socketPath));
  return addr;
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getEdenClient(
    w_string_piece rootPath,
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  return retryEStale([&] {
    auto addr = getEdenSocketAddress(rootPath);

    return make_unique<StreamingEdenServiceAsyncClient>(
        apache::thrift::HeaderClientChannel::newChannel(
            TAsyncSocket::newSocket(eb, addr)));
  });
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user.
 * This particular client uses the RSocketClientChannel channel that
 * is required to use the new thrift streaming protocol. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getRSocketEdenClient(
    w_string_piece rootPath,
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  return retryEStale([&] {
    auto addr = getEdenSocketAddress(rootPath);

    return make_unique<StreamingEdenServiceAsyncClient>(
        apache::thrift::RSocketClientChannel::newChannel(
            TAsyncSocket::UniquePtr(new TAsyncSocket(eb, addr))));
  });
}

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(
      const w_string& rootPath,
      const w_string& fullName,
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
  }

  Optional<FileInformation> stat() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::FullFileInformation);
      return folly::none;
    }
    return stat_;
  }

  Optional<size_t> size() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::Size);
      return folly::none;
    }
    return stat_->size;
  }

  Optional<struct timespec> accessedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return folly::none;
    }
    return stat_->atime;
  }

  Optional<struct timespec> modifiedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return folly::none;
    }
    return stat_->mtime;
  }

  Optional<struct timespec> changedTime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::StatTimeStamps);
      return folly::none;
    }
    return stat_->ctime;
  }

  w_string_piece baseName() override {
    return fullName_.piece().baseName();
  }

  w_string_piece dirName() override {
    return fullName_.piece().dirName();
  }

  void setExists(bool exists) noexcept {
    exists_ = exists;
    if (!exists) {
      stat_ = FileInformation::makeDeletedFileInformation();
    }
  }

  Optional<bool> exists() override {
    if (!exists_.has_value()) {
      accessorNeedsProperties(FileResult::Property::Exists);
      return folly::none;
    }
    return exists_;
  }

  Optional<w_string> readLink() override {
    if (symlinkTarget_.has_value()) {
      return symlinkTarget_;
    }
    accessorNeedsProperties(FileResult::Property::SymlinkTarget);
    return folly::none;
  }

  Optional<w_clock_t> ctime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::CTime);
      return folly::none;
    }
    return ctime_;
  }

  Optional<w_clock_t> otime() override {
    if (!stat_.has_value()) {
      accessorNeedsProperties(FileResult::Property::OTime);
      return folly::none;
    }
    return otime_;
  }

  Optional<FileResult::ContentHash> getContentSha1() override {
    if (!sha1_.has_value()) {
      accessorNeedsProperties(FileResult::Property::ContentSha1);
      return folly::none;
    }
    switch (sha1_->getType()) {
      // Copy thrift SHA1Result aka (std::string) into
      // watchman FileResult::ContentHash aka (std::array<uint8_t, 20>)
      case SHA1Result::Type::sha1: {
        auto& hash = sha1_->get_sha1();
        FileResult::ContentHash result;
        std::copy(hash.begin(), hash.end(), result.begin());

        return result;
      }

      // Thrift error occured
      case SHA1Result::Type::error: {
        auto& err = sha1_->get_error();
        throw std::system_error(
            err.errorCode_ref().value_unchecked(),
            std::generic_category(),
            err.message);
      }

      // Something is wrong with type union
      default:
        throw std::runtime_error(
            "Unknown thrift data for EdenFileResult::getContentSha1");
    }
  }

  void batchFetchProperties(
      const std::vector<std::unique_ptr<FileResult>>& files) override {
    std::vector<EdenFileResult*> getFileInformationFiles;
    std::vector<std::string> getFileInformationNames;

    std::vector<EdenFileResult*> getShaFiles;
    std::vector<std::string> getShaNames;

    std::vector<EdenFileResult*> getSymlinkFiles;

    for (auto& f : files) {
      auto& edenFile = dynamic_cast<EdenFileResult&>(*f.get());

      auto relName = edenFile.fullName_.piece();
      // Strip off the mount point prefix for the names we're going
      // to pass to eden.  The +1 is its trailing slash.
      relName.advance(root_path_.size() + 1);

      if (edenFile.neededProperties() & FileResult::Property::SymlinkTarget) {
        // We need to know if the node is a symlink
        edenFile.accessorNeedsProperties(FileResult::Property::FileDType);

        getSymlinkFiles.emplace_back(&edenFile);
      }

      if (edenFile.neededProperties() &
          (FileResult::Property::FileDType | FileResult::Property::CTime |
           FileResult::Property::OTime | FileResult::Property::Exists |
           FileResult::Property::Size | FileResult::Property::StatTimeStamps |
           FileResult::Property::FullFileInformation)) {
        getFileInformationFiles.emplace_back(&edenFile);
        getFileInformationNames.emplace_back(relName.data(), relName.size());
      }

      if (edenFile.neededProperties() & FileResult::Property::ContentSha1) {
        getShaFiles.emplace_back(&edenFile);
        getShaNames.emplace_back(relName.data(), relName.size());
      }

      // If we were to throw later in this method, we will have forgotten
      // the input set of properties, but it is ok: if we do decide to
      // re-evaluate after throwing, the accessors will set the mask up
      // accordingly and we'll end up calling back in here if needed.
      edenFile.clearNeededProperties();
    }

    auto client = getEdenClient(root_path_);
    loadFileInformation(
        client.get(), getFileInformationNames, getFileInformationFiles);

    // TODO: add eden bulk readlink call
    loadSymlinkTargets(client.get(), getSymlinkFiles);

    if (!getShaFiles.empty()) {
      std::vector<SHA1Result> sha1s;
      client->sync_getSHA1(sha1s, to<std::string>(root_path_), getShaNames);

      if (sha1s.size() != getShaFiles.size()) {
        watchman::log(
            ERR,
            "Requested SHA-1 of ",
            getShaFiles.size(),
            " but Eden returned ",
            sha1s.size(),
            " results -- ignoring");
      } else {
        auto sha1Iter = sha1s.begin();
        for (auto& edenFile : getShaFiles) {
          edenFile->sha1_ = *sha1Iter++;
        }
      }
    }
  }

 private:
  w_string root_path_;
  w_string fullName_;
  Optional<FileInformation> stat_;
  Optional<bool> exists_;
  w_clock_t ctime_;
  w_clock_t otime_;
  Optional<SHA1Result> sha1_;
  Optional<w_string> symlinkTarget_;

  // Read the symlink targets for each of the provided `files`.  The files
  // had SymlinkTarget set in neededProperties prior to clearing it in
  // the batchFetchProperties() method that calls us, so we know that
  // we unconditionally need to read these links.
  void loadSymlinkTargets(
      StreamingEdenServiceAsyncClient* client,
      const std::vector<EdenFileResult*>& files) {
    for (auto& edenFile : files) {
      if (!edenFile->stat_->isSymlink()) {
        // If this file is not a symlink then we immediately yield
        // a nullptr w_string instance rather than propagating an error.
        // This behavior is relied upon by the field rendering code and
        // checked in test_symlink.py.
        edenFile->symlinkTarget_ = w_string();
        continue;
      }
      edenFile->symlinkTarget_ = readSymbolicLink(edenFile->fullName_.c_str());
    }
  }

  void loadFileInformation(
      StreamingEdenServiceAsyncClient* client,
      const std::vector<std::string>& names,
      const std::vector<EdenFileResult*>& outFiles) {
    w_assert(
        names.size() == outFiles.size(), "names.size must == outFiles.size");
    if (names.empty()) {
      return;
    }

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, to<std::string>(root_path_), names);

    if (names.size() != info.size()) {
      watchman::log(
          ERR,
          "Requested file information of ",
          names.size(),
          " files but Eden returned information for ",
          info.size(),
          " files. Treating missing entries as missing files.");
    }

    auto infoIter = info.begin();
    for (auto& edenFileResult : outFiles) {
      if (infoIter == info.end()) {
        FileInformationOrError missingInfo;
        missingInfo.set_error(EdenError("Missing info"));
        edenFileResult->applyFileInformationOrError(missingInfo);
      } else {
        edenFileResult->applyFileInformationOrError(*infoIter);
        infoIter++;
      }
    }
  }

  void applyFileInformationOrError(const FileInformationOrError& infoOrErr) {
    if (infoOrErr.getType() == FileInformationOrError::Type::info) {
      FileInformation stat;

      stat.size = infoOrErr.get_info().size;
      stat.mode = infoOrErr.get_info().mode;
      stat.mtime.tv_sec = infoOrErr.get_info().mtime.seconds;
      stat.mtime.tv_nsec = infoOrErr.get_info().mtime.nanoSeconds;

      otime_.timestamp = ctime_.timestamp = stat.mtime.tv_sec;

      stat_ = std::move(stat);
      setExists(true);
    } else {
      setExists(false);
    }
  }
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
 * in the query. */
void filterOutPaths(std::vector<std::string>& fileNames, w_query_ctx* ctx) {
  fileNames.erase(
      std::remove_if(
          fileNames.begin(),
          fileNames.end(),
          [ctx](const std::string& name) {
            auto full = w_string::pathCat({ctx->root->root_path, name});

            if (!ctx->fileMatchesRelativeRoot(full)) {
              // Not in the desired area, so filter it out
              return true;
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

    std::array<folly::Future<SCM::StatusResult>, 2> futures{
        folly::via(
            &getThreadPool(),
            [this, hashA, hashB] {
              return getFilesChangedBetweenCommitsFromEden(hashA, hashB);
            }),
        folly::via(&getThreadPool(), [this, hashA, hashB] {
          return inner_->getFilesChangedBetweenCommits(hashA, hashB);
        })};
    auto resultPair = folly::collectAny(futures).get();
    return resultPair.second.value();
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

  std::chrono::time_point<std::chrono::system_clock> getCommitDate(
      w_string_piece commitId,
      w_string requestId = nullptr) const override {
    return inner_->getCommitDate(commitId, requestId);
  }

  std::vector<w_string> getCommitsPriorToAndIncluding(
      w_string_piece commitId,
      int numCommits,
      w_string requestId = nullptr) const override {
    return inner_->getCommitsPriorToAndIncluding(
        commitId, numCommits, requestId);
  }

  static std::unique_ptr<EdenWrappedSCM> wrap(std::unique_ptr<SCM> inner) {
    if (!inner) {
      return nullptr;
    }
    return make_unique<EdenWrappedSCM>(std::move(inner));
  }
};

/** Returns the files that match the glob. */
std::vector<std::string> callEdenGlobViaThrift(
    StreamingEdenServiceAsyncClient* client,
    const std::string& mountPoint,
    const std::vector<std::string>& globPatterns,
    bool includeDotfiles) {
  GlobParams params;
  params.set_mountPoint(mountPoint);
  params.set_globs(globPatterns);
  params.set_includeDotfiles(includeDotfiles);

  Glob glob;
  client->sync_globFiles(glob, params);
  return glob.get_matchingFiles();
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
  std::promise<void> subscribeReadyPromise_;
  std::shared_future<void> subscribeReadyFuture_;

 public:
  explicit EdenView(w_root_t* root)
      : root_path_(root->root_path),
        scm_(EdenWrappedSCM::wrap(SCM::scmForPath(root->root_path))),
        // Allow for 32 pairs of revs, with errors cached for 10 seconds
        filesBetweenCommitCache_(32, std::chrono::seconds(10)),
        mountPoint_(to<std::string>(root->root_path)),
        subscribeReadyFuture_(subscribeReadyPromise_.get_future()) {
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
        if (err.errorCode_ref().value_unchecked() != ERANGE) {
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

    for (auto& name : fileNames) {
      // a file is considered new if it was present in the created files
      // set returned from eden.
      bool isNew = createdFileNames.find(name) != createdFileNames.end();

      auto file = make_unique<EdenFileResult>(
          root_path_,
          w_string::pathCat({mountPoint_, name}),
          &resultPosition,
          isNew);

      if (ctx->since.clock.is_fresh_instance) {
        // Fresh instance queries only return data about files
        // that currently exist, and we know this to be true
        // here because our list of files comes from evaluating
        // a glob.
        file->setExists(true);
      }

      w_query_process_file(ctx->query, ctx, std::move(file));
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

    for (auto& name : fileNames) {
      auto file = make_unique<EdenFileResult>(
          root_path_, w_string::pathCat({mountPoint_, name}));

      // The results of a glob are known to exist
      file->setExists(true);

      w_query_process_file(ctx->query, ctx, std::move(file));
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
    auto noescape =
        query->query_spec.get_default("glob_noescape", json_false()).asBool();
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
    auto result = filesBetweenCommitCache_
                      .get(
                          key,
                          [this](const BetweenCommitKey& cacheKey) {
                            return folly::makeFuture(
                                getSCM()->getFilesChangedBetweenCommits(
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

  std::unique_ptr<StreamingEdenServiceAsyncClient> legacySubscribe(
      std::shared_ptr<w_root_t> root,
      SettleCallback& settleCallback,
      std::chrono::milliseconds& settleTimeout) {
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
        onUpdate, std::string(root->root_path.data(), root->root_path.size()));

    return client;
  }

  std::unique_ptr<StreamingEdenServiceAsyncClient> rSocketSubscribe(
      std::shared_ptr<w_root_t> root,
      SettleCallback& settleCallback,
      std::chrono::milliseconds& settleTimeout) {
    auto client = getRSocketEdenClient(root->root_path, &subscriberEventBase_);
    auto stream = client->sync_subscribeStreamTemporary(
        std::string(root->root_path.data(), root->root_path.size()));
    auto streamFuture =
        std::move(stream)
            .via(&subscriberEventBase_)
            .subscribe(
                [&settleCallback, this, root, settleTimeout](
                    const JournalPosition&) {
                  watchman::log(DBG, "Got subscription push from eden\n");
                  if (settleCallback.isScheduled()) {
                    watchman::log(DBG, "reschedule settle timeout\n");
                    settleCallback.cancelTimeout();
                  }
                  subscriberEventBase_.timer().scheduleTimeout(
                      &settleCallback, settleTimeout);

                  // We need to process cookie files with the lowest
                  // possible latency, so we consume that information now
                  checkCookies(root);
                },
                [this](folly::exception_wrapper e) {
                  auto reason = folly::exceptionStr(std::move(e));
                  watchman::log(
                      ERR,
                      "subscription stream ended: ",
                      w_string_piece(reason.data(), reason.size()),
                      ", cancel watch\n");
                  // We won't be called again, but we terminate the loop
                  // just to make sure.
                  subscriberEventBase_.terminateLoopSoon();
                })
            .futureJoin();
    return client;
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
      std::unique_ptr<StreamingEdenServiceAsyncClient> client;

      try {
        client = rSocketSubscribe(root, settleCallback, settleTimeout);
      } catch (const apache::thrift::TApplicationException& exc) {
        if (exc.getType() ==
            apache::thrift::TApplicationException::UNKNOWN_METHOD) {
          // Fall back to the older subscription stuff
          watchman::log(
              watchman::ERR,
              "Error while establishing rsocket subscription: ",
              exc.what(),
              ", falling back on legacy subscription\n");
        } else {
          throw;
        }
      } catch (const apache::thrift::transport::TTransportException& exc) {
        if (exc.getType() !=
            apache::thrift::transport::TTransportException::TIMED_OUT) {
          throw;
        }
        // This can happen when the running eden server is the -oss flavor
        // of the build.  rSocket doesn't appear to respond except with a
        // timeout, even if we retry or increase the timeout.  We have no
        // choice but to use the legacy subscription stream.
        watchman::log(
            watchman::ERR,
            "Error while establishing rsocket subscription; "
            "server does not appear to support rSocket: ",
            exc.what(),
            ", falling back on legacy subscription\n");
      }

      // Client should only be nullptr if we hit one of the fallback cases
      // above.
      if (!client) {
        client = legacySubscribe(root, settleCallback, settleTimeout);
      }

      // This will run until the stream ends
      watchman::log(watchman::DBG, "Started subscription thread loop\n");
      subscribeReadyPromise_.set_value();
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
    return subscribeReadyFuture_;
  }
};

std::shared_ptr<watchman::QueryableView> detectEden(w_root_t* root) {
  if (root->fs_type != "fuse" && root->fs_type != "osxfuse_eden") {
    throw std::runtime_error(to<std::string>("not a FUSE file system"));
  }

  auto edenRoot = readSymbolicLink(
      watchman::to<std::string>(root->root_path, "/.eden/root").c_str());
  if (edenRoot != root->root_path) {
    // We aren't at the root of the eden mount.
    // Throw a TerminalWatcherError to indicate that the Eden watcher is the
    // correct watcher type for this directory (so don't try other watcher
    // types), but that it can't be used due to an error.
    throw TerminalWatcherError(to<std::string>(
        "you may only watch from the root of an eden mount point. "
        "Try again using ",
        edenRoot));
  }

  // Given that the readlink() succeeded, assume this is an Eden mount.
  return std::make_shared<EdenView>(root);
}

} // namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // namespace watchman
