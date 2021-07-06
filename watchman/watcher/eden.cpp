/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <cpptoml.h> // @manual=fbsource//third-party/cpptoml:cpptoml
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>
#include "eden/fs/service/gen-cpp2/StreamingEdenService.h"
#include "watchman/ChildProcess.h"
#include "watchman/Errors.h"
#include "watchman/FSDetect.h"
#include "watchman/QueryableView.h"
#include "watchman/ThreadPool.h"
#include "watchman/scm/SCM.h"
#include "watchman/thirdparty/wildmatch/wildmatch.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watcher/WatcherRegistry.h"
#include "watchman/watchman.h"

using apache::thrift::TApplicationException;
using facebook::eden::EdenError;
using facebook::eden::EntryInformation;
using facebook::eden::EntryInformationOrError;
using facebook::eden::FileDelta;
using facebook::eden::FileInformation;
using facebook::eden::FileInformationOrError;
using facebook::eden::Glob;
using facebook::eden::GlobParams;
using facebook::eden::JournalPosition;
using facebook::eden::SHA1Result;
using facebook::eden::StreamingEdenServiceAsyncClient;
using folly::AsyncSocket;
using folly::Optional;
using folly::to;
using std::make_unique;

namespace {
using EdenDtype = facebook::eden::Dtype;
using watchman::DType;

DType getDTypeFromEden(EdenDtype dtype) {
  // TODO: Eden guarantees that dtypes have consistent values on all platforms,
  // including Windows. If we made Watchman guarantee that too, this could be
  // replaced with a static_cast.

  switch (dtype) {
    case EdenDtype::UNKNOWN:
      return DType::Unknown;
    case EdenDtype::FIFO:
      return DType::Fifo;
    case EdenDtype::CHAR:
      return DType::Char;
    case EdenDtype::DIR:
      return DType::Dir;
    case EdenDtype::BLOCK:
      return DType::Block;
    case EdenDtype::REGULAR:
      return DType::Regular;
    case EdenDtype::LINK:
      return DType::Symlink;
    case EdenDtype::SOCKET:
      return DType::Socket;
    case EdenDtype::WHITEOUT:
      return DType::Whiteout;
  }
  return DType::Unknown;
}
} // namespace

namespace watchman {
namespace {
struct NameAndDType {
  std::string name;
  DType dtype;

  explicit NameAndDType(const std::string& name, DType dtype = DType::Unknown)
      : name(name), dtype(dtype) {}
};

/** This is a helper for settling out subscription events.
 * We have a single instance of the callback object that we schedule
 * each time we get an update from the eden server.  If we are already
 * scheduled we will cancel it and reschedule it.
 */
class SettleCallback : public folly::HHWheelTimer::Callback {
 public:
  SettleCallback(
      folly::EventBase* eventBase,
      std::shared_ptr<watchman_root> root)
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
  std::shared_ptr<watchman_root> root_;
};

// Resolve the eden socket; On POSIX systems we use the .eden dir that is
// present in every dir of an eden mount to locate the symlink to the socket.
// On Windows systems, .eden is only present in the repo root and contains
// the toml config file with the path to the socket.
std::string resolveSocketPath(w_string_piece rootPath) {
#ifdef _WIN32
  auto configPath = to<std::string>(rootPath, "/.eden/config");
  auto config = cpptoml::parse_file(configPath);

  return *config->get_qualified_as<std::string>("Config.socket");
#else
  auto path = to<std::string>(rootPath, "/.eden/socket");
  // It is important to resolve the link because the path in the eden mount
  // may exceed the maximum permitted unix domain socket path length.
  // This is actually how things our in our integration test environment.
  return to<std::string>(readSymbolicLink(path.c_str()));
#endif
}

folly::SocketAddress getEdenSocketAddress(w_string_piece rootPath) {
  folly::SocketAddress addr;

  auto socketPath = resolveSocketPath(rootPath);
  addr.setFromPath(to<std::string>(socketPath));
  return addr;
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getEdenClient(
    w_string_piece rootPath,
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  auto addr = getEdenSocketAddress(rootPath);

  return make_unique<StreamingEdenServiceAsyncClient>(
      apache::thrift::HeaderClientChannel::newChannel(
          AsyncSocket::newSocket(eb, addr)));
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user.
 * This particular client uses the RocketClientChannel channel that
 * is required to use the new thrift streaming protocol. */
std::unique_ptr<StreamingEdenServiceAsyncClient> getRocketEdenClient(
    w_string_piece rootPath,
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  auto addr = getEdenSocketAddress(rootPath);

  return make_unique<StreamingEdenServiceAsyncClient>(
      apache::thrift::RocketClientChannel::newChannel(
          AsyncSocket::UniquePtr(new AsyncSocket(eb, addr))));
}

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(
      const w_string& rootPath,
      const w_string& fullName,
      JournalPosition* position = nullptr,
      bool isNew = false,
      DType dtype = DType::Unknown)
      : root_path_(rootPath), fullName_(fullName), dtype_(dtype) {
    otime_.ticks = ctime_.ticks = 0;
    otime_.timestamp = ctime_.timestamp = 0;
    if (position) {
      otime_.ticks = *position->sequenceNumber_ref();
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

  Optional<watchman::DType> dtype() override {
    // We're using Unknown as the default value to avoid also wrapping
    // this value up in an Optional in our internal storage.
    // In theory this is ambiguous, but in practice Eden will never
    // return Unknown for dtype values so this is safe to use with
    // impunity.
    if (dtype_ != DType::Unknown) {
      return dtype_;
    }
    if (stat_.has_value()) {
      return stat_->dtype();
    }
    accessorNeedsProperties(FileResult::Property::FileDType);
    return folly::none;
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
    return ctime_;
  }

  Optional<w_clock_t> otime() override {
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
        XCHECK(err.errorCode_ref());
        throw std::system_error(
            *err.errorCode_ref(), std::generic_category(), *err.message_ref());
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
    // If only dtype and exists are needed, Eden has a cheaper API for
    // retrieving them.
    bool onlyEntryInfoNeeded = true;

    std::vector<EdenFileResult*> getShaFiles;
    std::vector<std::string> getShaNames;

    std::vector<EdenFileResult*> getSymlinkFiles;

    for (auto& f : files) {
      auto& edenFile = dynamic_cast<EdenFileResult&>(*f);

      auto relName = edenFile.fullName_.piece();

      if (root_path_ == edenFile.fullName_) {
        // The root tree inode has changed
        relName = "";
      } else {
        // Strip off the mount point prefix for the names we're going
        // to pass to eden.  The +1 is its trailing slash.
        relName.advance(root_path_.size() + 1);
      }

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

        if (edenFile.neededProperties() &
            ~(FileResult::Property::FileDType | FileResult::Property::Exists)) {
          // We could maintain two lists and call both getFileInformation and
          // getEntryInformation in parallel, but in practice the set of
          // properties should usually be the same across all files.
          onlyEntryInfoNeeded = false;
        }
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
        client.get(),
        root_path_,
        getFileInformationNames,
        getFileInformationFiles,
        onlyEntryInfoNeeded);

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
  DType dtype_{DType::Unknown};

  // Read the symlink targets for each of the provided `files`.  The files
  // had SymlinkTarget set in neededProperties prior to clearing it in
  // the batchFetchProperties() method that calls us, so we know that
  // we unconditionally need to read these links.
  static void loadSymlinkTargets(
      StreamingEdenServiceAsyncClient*,
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

  static void loadFileInformation(
      StreamingEdenServiceAsyncClient* client,
      const w_string& rootPath,
      const std::vector<std::string>& names,
      const std::vector<EdenFileResult*>& outFiles,
      bool onlyEntryInfoNeeded) {
    w_assert(
        names.size() == outFiles.size(), "names.size must == outFiles.size");
    if (names.empty()) {
      return;
    }

    auto applyResults = [&](const auto& edenInfo) {
      if (names.size() != edenInfo.size()) {
        watchman::log(
            ERR,
            "Requested file information of ",
            names.size(),
            " files but Eden returned information for ",
            edenInfo.size(),
            " files. Treating missing entries as missing files.");
      }

      auto infoIter = edenInfo.begin();
      for (auto& edenFileResult : outFiles) {
        if (infoIter == edenInfo.end()) {
          edenFileResult->setExists(false);
        } else {
          edenFileResult->applyInformationOrError(*infoIter);
          ++infoIter;
        }
      }
    };

    if (onlyEntryInfoNeeded) {
      std::vector<EntryInformationOrError> info;
      try {
        client->sync_getEntryInformation(
            info, to<std::string>(rootPath), names);
        applyResults(info);
        return;
      } catch (const TApplicationException& ex) {
        if (TApplicationException::UNKNOWN_METHOD != ex.getType()) {
          throw;
        }
        // getEntryInformation is not available in this version of
        // Eden. Fall back to the older, more expensive
        // getFileInformation below.
      }
    }

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, to<std::string>(rootPath), names);
    applyResults(info);
  }

  void applyInformationOrError(const EntryInformationOrError& infoOrErr) {
    if (infoOrErr.getType() == EntryInformationOrError::Type::info) {
      dtype_ = getDTypeFromEden(*infoOrErr.get_info().dtype_ref());
      setExists(true);
    } else {
      setExists(false);
    }
  }

  void applyInformationOrError(const FileInformationOrError& infoOrErr) {
    if (infoOrErr.getType() == FileInformationOrError::Type::info) {
      FileInformation stat;

      stat.size = *infoOrErr.get_info().size_ref();
      stat.mode = *infoOrErr.get_info().mode_ref();
      stat.mtime.tv_sec = *infoOrErr.get_info().mtime_ref()->seconds_ref();
      stat.mtime.tv_nsec = *infoOrErr.get_info().mtime_ref()->nanoSeconds_ref();

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
void filterOutPaths(std::vector<NameAndDType>& files, w_query_ctx* ctx) {
  files.erase(
      std::remove_if(
          files.begin(),
          files.end(),
          [ctx](const NameAndDType& item) {
            auto full = w_string::pathCat({ctx->root->root_path, item.name});

            if (!ctx->fileMatchesRelativeRoot(full)) {
              // Not in the desired area, so filter it out
              return true;
            }

            return ctx->root->ignore.isIgnored(full.data(), full.size());
          }),
      files.end());
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
      std::vector<std::string> commits,
      w_string /* requestId */ = nullptr) const override {
    return inner_->getFilesChangedBetweenCommits(std::move(commits));
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

void appendGlobResultToNameAndDTypeVec(
    std::vector<NameAndDType>& results,
    Glob&& glob) {
  size_t i = 0;
  size_t numDTypes = glob.get_dtypes().size();

  for (auto& name : glob.get_matchingFiles()) {
    // The server may not support dtypes, so this list may be empty.
    // This cast is OK because eden returns the system dependent bits to us, and
    // our DType enum is declared in terms of those bits
    auto dtype = i < numDTypes ? static_cast<DType>(glob.get_dtypes()[i])
                               : DType::Unknown;
    results.emplace_back(name, dtype);
    ++i;
  }
}

/** Returns the files that match the glob. */
std::vector<NameAndDType> globNameAndDType(
    StreamingEdenServiceAsyncClient* client,
    const std::string& mountPoint,
    const std::vector<std::string>& globPatterns,
    bool includeDotfiles,
    bool splitGlobPattern = false) {
  // TODO(xavierd): Once the config: "eden_split_glob_pattern" is rolled out
  // everywhere, remove this code.
  if (splitGlobPattern && globPatterns.size() > 1) {
    folly::DrivableExecutor* executor =
        folly::EventBaseManager::get()->getEventBase();

    std::vector<folly::Future<Glob>> globFutures;
    globFutures.reserve(globPatterns.size());
    for (const std::string& globPattern : globPatterns) {
      GlobParams params;
      params.mountPoint_ref() = mountPoint;
      params.globs_ref() = std::vector<std::string>{globPattern};
      params.includeDotfiles_ref() = includeDotfiles;
      params.wantDtype_ref() = true;

      globFutures.emplace_back(
          client->semifuture_globFiles(params).via(executor));
    }

    std::vector<NameAndDType> allResults;
    for (folly::Future<Glob>& globFuture : globFutures) {
      appendGlobResultToNameAndDTypeVec(
          allResults, std::move(globFuture).getVia(executor));
    }
    return allResults;
  } else {
    GlobParams params;
    params.mountPoint_ref() = mountPoint;
    params.globs_ref() = globPatterns;
    params.includeDotfiles_ref() = includeDotfiles;
    params.wantDtype_ref() = true;

    Glob glob;
    client->sync_globFiles(glob, params);
    std::vector<NameAndDType> result;
    appendGlobResultToNameAndDTypeVec(result, std::move(glob));
    return result;
  }
}

class EdenView final : public QueryableView {
  w_string root_path_;
  // The source control system that we detected during initialization
  mutable std::unique_ptr<EdenWrappedSCM> scm_;
  folly::EventBase subscriberEventBase_;
  JournalPosition lastCookiePosition_;
  std::string mountPoint_;
  std::promise<void> subscribeReadyPromise_;
  std::shared_future<void> subscribeReadyFuture_;
  bool splitGlobPattern_;

 public:
  explicit EdenView(watchman_root* root)
      : root_path_(root->root_path),
        scm_(EdenWrappedSCM::wrap(SCM::scmForPath(root->root_path))),
        mountPoint_(to<std::string>(root->root_path)),
        subscribeReadyFuture_(subscribeReadyPromise_.get_future()),
        splitGlobPattern_(
            root->config.getBool("eden_split_glob_pattern", false)) {
    // Get the current journal position so that we can keep track of
    // cookie file changes
    auto client = getEdenClient(root_path_);
    client->sync_getCurrentJournalPosition(lastCookiePosition_, mountPoint_);
    // We don't run an iothread so we never need to crawl and
    // thus should be considered to have "completed" the initial
    // exploration of the root
    root->inner.done_initial = true;
    auto crawlInfo = root->recrawlInfo.wlock();
    crawlInfo->shouldRecrawl = false;
    crawlInfo->crawlStart = std::chrono::steady_clock::now();
    crawlInfo->crawlFinish = crawlInfo->crawlStart;
  }

  void timeGenerator(w_query* query, struct w_query_ctx* ctx) const override {
    ctx->generationStarted();
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
        return std::vector<NameAndDType>();
      }

      std::string globPattern;
      if (ctx->query->relative_root) {
        w_string_piece rel(ctx->query->relative_root);
        rel.advance(ctx->root->root_path.size() + 1);
        globPattern.append(rel.data(), rel.size());
        globPattern.append("/");
      }
      globPattern.append("**");
      return globNameAndDType(
          client.get(),
          mountPoint_,
          std::vector<std::string>{globPattern},
          includeDotfiles);
    };

    std::vector<NameAndDType> fileInfo;
    // We use the list of created files to synthesize the "new" field
    // in the file results
    std::unordered_set<std::string> createdFileNames;

    if (ctx->since.clock.is_fresh_instance) {
      // Earlier in the processing flow, we decided that the rootNumber
      // didn't match the current root which means that eden was restarted.
      // We need to translate this to a fresh instance result set and
      // return a list of all possible matching files.
      client->sync_getCurrentJournalPosition(resultPosition, mountPoint_);
      fileInfo = getAllFiles();
    } else {
      // Query eden to fill in the mountGeneration field.
      JournalPosition position;
      client->sync_getCurrentJournalPosition(position, mountPoint_);
      // dial back to the sequence number from the query
      *position.sequenceNumber_ref() = ctx->since.clock.ticks;

      // Now we can get the change journal from eden
      try {
        client->sync_getFilesChangedSince(delta, mountPoint_, position);

        createdFileNames.insert(
            delta.createdPaths_ref()->begin(), delta.createdPaths_ref()->end());

        // The list of changed files is the union of the created, added,
        // and removed sets returned from eden in list form.
        for (auto& name : *delta.changedPaths_ref()) {
          fileInfo.emplace_back(NameAndDType(std::move(name)));
        }
        for (auto& name : *delta.removedPaths_ref()) {
          fileInfo.emplace_back(NameAndDType(std::move(name)));
        }
        for (auto& name : *delta.createdPaths_ref()) {
          fileInfo.emplace_back(NameAndDType(std::move(name)));
        }

        bool didChangeCommits = delta.snapshotTransitions_ref()->size() >= 2 ||
            (delta.fromPosition_ref()->snapshotHash_ref() !=
             delta.toPosition_ref()->snapshotHash_ref());

        if (scm_ && didChangeCommits) {
          // Check whether they checked out a new commit or reset the commit to
          // a different hash.  We interrogate source control to discover
          // the set of changed files between those hashes, and then
          // add in any paths that may have changed around snapshot hash
          // changes events;  These are files whose status cannot be
          // determined purely from source control operations.

          std::unordered_set<std::string> mergedFileList;
          for (auto& info : fileInfo) {
            mergedFileList.insert(info.name);
          }

          SCM::StatusResult changedBetweenCommits;
          if (delta.snapshotTransitions_ref()->empty()) {
            auto fromHash =
                folly::hexlify(*delta.fromPosition_ref()->snapshotHash_ref());
            auto toHash =
                folly::hexlify(*delta.toPosition_ref()->snapshotHash_ref());

            // Legacy path: this (incorrectly) ignores any commit transitions
            // between the initial commit hash and the final commit hash.
            log(ERR,
                "since ",
                *position.sequenceNumber_ref(),
                " we changed commit hashes from ",
                fromHash,
                " to ",
                toHash,
                "\n");

            std::vector<std::string> commits{
                std::move(fromHash), std::move(toHash)};
            changedBetweenCommits =
                getSCM()->getFilesChangedBetweenCommits(std::move(commits));
          } else if (delta.snapshotTransitions_ref()->size() >= 2) {
            std::vector<std::string> commits;
            commits.reserve(delta.snapshotTransitions_ref()->size());
            for (auto& hash : *delta.snapshotTransitions_ref()) {
              commits.push_back(folly::hexlify(hash));
            }
            log(ERR,
                "since ",
                *position.sequenceNumber_ref(),
                " we changed commit hashes ",
                folly::join(" -> ", commits),
                "\n");
            changedBetweenCommits =
                getSCM()->getFilesChangedBetweenCommits(std::move(commits));
          }

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
              std::make_move_iterator(delta.uncleanPaths_ref()->begin()),
              std::make_move_iterator(delta.uncleanPaths_ref()->end()));

          // Replace the list of fileNames with the de-duped set
          // of names we've extracted from source control
          fileInfo.clear();
          for (auto name : mergedFileList) {
            fileInfo.emplace_back(std::move(name));
          }
        }

        resultPosition = *delta.toPosition_ref();
        watchman::log(
            watchman::DBG,
            "wanted from ",
            *position.sequenceNumber_ref(),
            " result delta from ",
            *delta.fromPosition_ref()->sequenceNumber_ref(),
            " to ",
            *delta.toPosition_ref()->sequenceNumber_ref(),
            " with ",
            fileInfo.size(),
            " changed files\n");
      } catch (const EdenError& err) {
        // ERANGE: mountGeneration differs
        // EDOM: journal was truncated.
        // For other situations we let the error propagate.
        XCHECK(err.errorCode_ref());
        if (*err.errorCode_ref() != ERANGE && *err.errorCode_ref() != EDOM) {
          throw;
        }
        // mountGeneration differs, or journal was truncated,
        // so treat this as equivalent to a fresh instance result
        ctx->since.clock.is_fresh_instance = true;
        client->sync_getCurrentJournalPosition(resultPosition, mountPoint_);
        fileInfo = getAllFiles();
      } catch (const SCMError& err) {
        // Most likely this means a checkout occurred but we encountered
        // an error trying to get the list of files changed between the two
        // commits.  Generate a fresh instance result since we were unable
        // to compute the list of files changed.
        watchman::log(
            ERR,
            "SCM error while processing EdenFS journal update: ",
            err.what(),
            "\n");
        ctx->since.clock.is_fresh_instance = true;
        client->sync_getCurrentJournalPosition(resultPosition, mountPoint_);
        fileInfo = getAllFiles();
      }
    }

    // Filter out any ignored files
    filterOutPaths(fileInfo, ctx);

    for (auto& item : fileInfo) {
      // a file is considered new if it was present in the created files
      // set returned from eden.
      bool isNew = createdFileNames.find(item.name) != createdFileNames.end();

      auto file = make_unique<EdenFileResult>(
          root_path_,
          w_string::pathCat({mountPoint_, item.name}),
          &resultPosition,
          isNew,
          item.dtype);

      if (ctx->since.clock.is_fresh_instance) {
        // Fresh instance queries only return data about files
        // that currently exist, and we know this to be true
        // here because our list of files comes from evaluating
        // a glob.
        file->setExists(true);
      }

      w_query_process_file(ctx->query, ctx, std::move(file));
    }

    ctx->bumpNumWalked(fileInfo.size());
  }

  void syncToNow(
      const std::shared_ptr<watchman_root>&,
      std::chrono::milliseconds) override {}

  void executeGlobBasedQuery(
      const std::vector<std::string>& globStrings,
      w_query* query,
      struct w_query_ctx* ctx) const {
    auto client = getEdenClient(ctx->root->root_path);

    auto includeDotfiles = (query->glob_flags & WM_PERIOD) == 0;
    auto fileInfo = globNameAndDType(
        client.get(),
        mountPoint_,
        globStrings,
        includeDotfiles,
        splitGlobPattern_);

    // Filter out any ignored files
    filterOutPaths(fileInfo, ctx);

    for (auto& item : fileInfo) {
      auto file = make_unique<EdenFileResult>(
          root_path_,
          w_string::pathCat({mountPoint_, item.name}),
          /* position=*/nullptr,
          /*isNew=*/false,
          item.dtype);

      // The results of a glob are known to exist
      file->setExists(true);

      w_query_process_file(ctx->query, ctx, std::move(file));
    }

    ctx->bumpNumWalked(fileInfo.size());
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
    ctx->generationStarted();
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    // Translate the path list into a list of globs
    for (auto& path : *query->paths) {
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
    if (!query->glob_tree) {
      // If we are called via the codepath in the query evaluator that
      // just speculatively executes queries then `glob` may not be
      // present; short-circuit in that case.
      return;
    }

    ctx->generationStarted();
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    auto rel = computeRelativePathPiece(ctx);

    std::vector<std::string> globStrings;
    for (auto& glob : query->glob_tree->unparse()) {
      globStrings.emplace_back(to<std::string>(w_string::pathCat({rel, glob})));
    }

    // More glob flags/functionality:
    auto noescape = bool(query->glob_flags & WM_NOESCAPE);
    if (noescape) {
      throw QueryExecError(
          "glob_noescape is not supported for the eden watcher");
    }
    executeGlobBasedQuery(globStrings, query, ctx);
  }

  void allFilesGenerator(w_query* query, struct w_query_ctx* ctx)
      const override {
    ctx->generationStarted();
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
    return ClockPosition(
        *position.mountGeneration_ref(), *position.sequenceNumber_ref());
  }

  w_string getCurrentClockString() const override {
    return getMostRecentRootNumberAndTickValue().toClockString();
  }

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& /*fileNames*/) const override {
    return false;
  }

  SCM* getSCM() const override {
    return scm_.get();
  }

  void startThreads(const std::shared_ptr<watchman_root>& root) override {
    auto self = shared_from_this();
    std::thread thr([self, this, root]() { subscriberThread(root); });
    thr.detach();
  }

  void signalThreads() override {
    subscriberEventBase_.terminateLoopSoon();
  }

  json_ref getWatcherDebugInfo() const override {
    return json_null();
  }

  void clearWatcherDebugInfo() override {}

  // Called by the subscriberThread to scan for cookie file creation
  // events.  These are used to manage sequencing for state-enter and
  // state-leave in eden.
  void checkCookies(const std::shared_ptr<watchman_root>& root) {
    // Obtain the list of changes since our last request, or since we started
    // up the watcher (we set the initial value of lastCookiePosition_ during
    // construction).
    try {
      FileDelta delta;
      auto client = getEdenClient(root_path_);
      client->sync_getFilesChangedSince(
          delta, mountPoint_, lastCookiePosition_);

      // TODO: in the future it would be nice to compute the paths in a loop
      // first, and then add a bulk CookieSync::notifyCookies() method to avoid
      // locking and unlocking its internal mutex so frequently.
      for (auto& file : *delta.createdPaths_ref()) {
        auto full = w_string::pathCat({root_path_, file});
        root->cookies.notifyCookie(full);
      }

      // Remember this position for subsequent calls
      lastCookiePosition_ = *delta.toPosition_ref();
    } catch (const EdenError& err) {
      // EDOM is journal truncation, which we can recover from.
      // Other errors (including ERANGE/mountGeneration changed)
      // are not recoverable, so let them propagate.
      XCHECK(err.errorCode_ref());
      if (*err.errorCode_ref() != EDOM) {
        throw;
      }
      // Journal was truncated: we can remain connected and have continuity
      // with the Journal sequence numbers, but we may have missed cookie
      // file events, so let's abort all currently outstanding cookies. The
      // cookie sync mechanism will retry if there is sufficient time remaining
      // in their individual retry schedule(s).
      root->cookies.abortAllCookies();
    }
  }

  std::unique_ptr<StreamingEdenServiceAsyncClient> rocketSubscribe(
      std::shared_ptr<watchman_root> root,
      SettleCallback& settleCallback,
      std::chrono::milliseconds& settleTimeout) {
    auto client = getRocketEdenClient(root->root_path, &subscriberEventBase_);
    auto stream = client->sync_subscribeStreamTemporary(
        std::string(root->root_path.data(), root->root_path.size()));
    std::move(stream)
        .subscribeExTry(
            &subscriberEventBase_,
            [&settleCallback, this, root, settleTimeout](
                folly::Try<JournalPosition>&& t) {
              if (t.hasValue()) {
                try {
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
                } catch (const std::exception& exc) {
                  watchman::log(
                      ERR,
                      "Exception while processing eden subscription: ",
                      exc.what(),
                      ": cancel watch\n");
                  subscriberEventBase_.terminateLoopSoon();
                }
              } else {
                auto reason = t.hasException()
                    ? folly::exceptionStr(std::move(t.exception()))
                    : "controlled shutdown";
                watchman::log(
                    ERR,
                    "subscription stream ended: ",
                    w_string_piece(reason.data(), reason.size()),
                    ", cancel watch\n");
                // We won't be called again, but we terminate the loop
                // just to make sure.
                subscriberEventBase_.terminateLoopSoon();
              }
            })
        .detach();
    return client;
  }

  // This is the thread that we use to listen to the stream of
  // changes coming in from the Eden server
  void subscriberThread(std::shared_ptr<watchman_root> root) noexcept {
    SCOPE_EXIT {
      // ensure that the root gets torn down,
      // otherwise we'd leave it in a broken state.
      root->cancel();
    };

    w_set_thread_name("edensub ", root->root_path);
    watchman::log(watchman::DBG, "Started subscription thread\n");

    try {
      // Prepare the callback
      SettleCallback settleCallback(&subscriberEventBase_, root);
      // Figure out the correct value for settling
      std::chrono::milliseconds settleTimeout(root->trigger_settle);
      std::unique_ptr<StreamingEdenServiceAsyncClient> client;

      client = rocketSubscribe(root, settleCallback, settleTimeout);

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
      const std::shared_ptr<watchman_root>& /*root*/) override {
    return subscribeReadyFuture_;
  }
};

#ifdef _WIN32
// Test if EdenFS is stopped for the given path.
bool isEdenStopped(w_string root) {
  static const w_string_piece kStar{"*"};
  static const w_string_piece kNonExistencePath{"EDEN_TEST_NON_EXISTENCE_PATH"};
  auto queryRaw = w_string::pathCat({root, kNonExistencePath, kStar});
  auto query = queryRaw.normalizeSeparators();
  std::wstring wquery = query.piece().asWideUNC();
  WIN32_FIND_DATAW ffd;

  auto find = FindFirstFileW(wquery.c_str(), &ffd);
  SCOPE_EXIT {
    if (find != INVALID_HANDLE_VALUE) {
      FindClose(find);
    }
  };

  auto lastError = GetLastError();

  // When EdenFS is not running, `FindFirstFile` will fail with this error
  // since it can't reach EdenFS to query directory information.
  if (find == INVALID_HANDLE_VALUE &&
      lastError == ERROR_FILE_SYSTEM_VIRTUALIZATION_UNAVAILABLE) {
    watchman::log(watchman::DBG, "edenfs is NOT RUNNING\n");
    return true;
  }

  watchman::log(watchman::DBG, "edenfs is RUNNING\n");
  return false;
}
#endif

std::shared_ptr<watchman::QueryableView> detectEden(watchman_root* root) {
#ifdef _WIN32
  static const w_string_piece kDotEden{".eden"};
  auto edenRoot = findFileInDirTree(root->root_path, {kDotEden});
  if (edenRoot) {
    if (isEdenStopped(root->root_path)) {
      throw TerminalWatcherError(to<std::string>(
          root->root_path,
          " appears to be an offline EdenFS mount. "
          "Try running `edenfsctl start` to bring it back online and "
          "then retry your watch"));
    }

    auto homeDotEdenRaw = w_string::pathCat({getenv("USERPROFILE"), kDotEden});
    auto homeDotEden = homeDotEdenRaw.normalizeSeparators();

    if (edenRoot == homeDotEden) {
      throw std::runtime_error(to<std::string>(
          "Not considering HOME/.eden as a valid Eden repo (found ",
          edenRoot,
          ")"));
    }
    try {
      return std::make_shared<EdenView>(root);
    } catch (const std::exception& exc) {
      throw TerminalWatcherError(to<std::string>(
          "Failed to initialize eden watcher, and since this is an Eden "
          "repo, will not allow falling back to another watcher.  Error was: ",
          exc.what()));
    }
  }

  throw std::runtime_error(
      to<std::string>("Not an Eden clone: ", root->root_path));

#else
  if (!is_edenfs_fs_type(root->fs_type) && root->fs_type != "fuse" &&
      root->fs_type != "osxfuse_eden" && root->fs_type != "macfuse_eden" &&
      root->fs_type != "edenfs_eden") {
    // Not an active EdenFS mount.  Perhaps it isn't mounted yet?
    auto readme = to<std::string>(root->root_path, "/README_EDEN.txt");
    try {
      (void)getFileInformation(readme.c_str());
    } catch (const std::exception&) {
      // We don't really care if the readme doesn't exist or is inaccessible,
      // we just wanted to do a best effort check for the readme file.
      // If we can't access it, we're still not in a position to treat
      // this as an EdenFS mount so record the issue and allow falling
      // back to one of the other watchers.
      throw std::runtime_error(
          to<std::string>(root->fs_type, " is not a FUSE file system"));
    }

    // If we get here, then the readme file/symlink exists.
    // If the readme exists then this is an offline eden mount.
    // We can't watch it using this watcher in its current state,
    // and we don't want to allow falling back to inotify as that
    // will be horribly slow.
    throw TerminalWatcherError(to<std::string>(
        root->root_path,
        " appears to be an offline EdenFS mount. "
        "Try running `eden doctor` to bring it back online and "
        "then retry your watch"));
  }

  auto edenRoot =
      readSymbolicLink(to<std::string>(root->root_path, "/.eden/root").c_str());
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
#endif
  // Given that the readlink() succeeded, assume this is an Eden mount.
  return std::make_shared<EdenView>(root);
}

} // namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // namespace watchman
