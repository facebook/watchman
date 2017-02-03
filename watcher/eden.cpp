/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include "eden/fs/service/gen-cpp2/EdenService.h"
#include "watchman.h"

#include "QueryableView.h"
#include "ThreadPool.h"

using facebook::eden::EdenServiceAsyncClient;
using facebook::eden::FileInformationOrError;
using facebook::eden::FileInformation;
using facebook::eden::JournalPosition;
using facebook::eden::FileDelta;
using facebook::eden::EdenError;

namespace watchman {
namespace {

folly::SocketAddress getEdenServerSocketAddress() {
  folly::SocketAddress addr;
  // In the future, eden will need to provide a well-supported way to locate
  // this socket, as the "local" path component here is a FB-specific default.
  auto path = folly::to<std::string>(getenv("HOME"), "/local/.eden/socket");
  addr.setFromPath(path);
  return addr;
}

/** Create a thrift client that will connect to the eden server associated
 * with the current user. */
std::unique_ptr<EdenServiceAsyncClient> getEdenClient(
    folly::EventBase* eb = folly::EventBaseManager::get()->getEventBase()) {
  return std::make_unique<EdenServiceAsyncClient>(
      apache::thrift::HeaderClientChannel::newChannel(
          apache::thrift::async::TAsyncSocket::newSocket(
              eb, getEdenServerSocketAddress())));
}

class EdenFileResult : public FileResult {
 public:
  EdenFileResult(
      const FileInformationOrError& infoOrErr,
      const w_string& fullName,
      JournalPosition* position = nullptr)
      : fullName_(fullName) {
    otime_.ticks = ctime_.ticks = 0;
    otime_.timestamp = ctime_.timestamp = 0;
    if (position) {
      otime_.ticks = ctime_.ticks = position->sequenceNumber;
    }

    if (infoOrErr.getType() == FileInformationOrError::Type::info) {
      stat_.size = infoOrErr.get_info().size;
      stat_.mode = infoOrErr.get_info().mode;
      stat_.mtime.tv_sec = infoOrErr.get_info().mtime.seconds;
      stat_.mtime.tv_nsec = infoOrErr.get_info().mtime.nanoSeconds;

      otime_.timestamp = ctime_.timestamp = stat_.mtime.tv_sec;

      exists_ = true;
    } else {
      exists_ = false;
      memset(&stat_, 0, sizeof(stat_));
    }
  }

  const watchman_stat& stat() const override {
    return stat_;
  }

  w_string_piece baseName() const override {
    return fullName_.piece().baseName();
  }

  w_string_piece dirName() override {
    return fullName_.piece().dirName();
  }

  bool exists() const override {
    return exists_;
  }

  Future<w_string> readLink() const override {
    return makeFuture<w_string>(nullptr);
  }

  const w_clock_t& ctime() const override {
    return ctime_;
  }
  const w_clock_t& otime() const override {
    return otime_;
  }

 private:
  w_string fullName_;
  watchman_stat stat_;
  bool exists_;
  w_clock_t ctime_;
  w_clock_t otime_;
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

class EdenView : public QueryableView {
  w_string root_path_;

 public:
  explicit EdenView(w_root_t* root) {
    root_path_ = root->root_path;
  }

  bool timeGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    auto client = getEdenClient();
    bool result = true;
    auto mountPoint = to<std::string>(ctx->root->root_path);

    FileDelta delta;
    JournalPosition resultPosition;

    if (ctx->since.is_timestamp) {
      throw std::runtime_error(
          "timestamp based since queries are not supported with eden");
    }

    // This is the fall back for a fresh instance result set.
    // There are two different code paths that may need this, so
    // it is broken out as a lambda.
    auto getAllFiles = [ctx, &client, &mountPoint]() {
      std::vector<std::string> fileNames;

      if (ctx->query->empty_on_fresh_instance) {
        // Avoid a full tree walk if we don't need it!
        return fileNames;
      }

      std::string globPattern;
      if (ctx->query->relative_root) {
        w_string_piece rel(ctx->query->relative_root);
        rel.advance(ctx->root->root_path.size() + 1);
        globPattern.append(rel.data(), rel.size());
        globPattern.append("/");
      }
      globPattern.append("**");
      client->sync_glob(
          fileNames, mountPoint, std::vector<std::string>{globPattern});
      return fileNames;
    };

    std::vector<std::string> fileNames;
    if (ctx->since.clock.is_fresh_instance) {
      // Earlier in the processing flow, we decided that the rootNumber
      // didn't match the current root which means that eden was restarted.
      // We need to translate this to a fresh instance result set and
      // return a list of all possible matching files.
      client->sync_getCurrentJournalPosition(resultPosition, mountPoint);
      fileNames = getAllFiles();
    } else {
      // Query eden to fill in the mountGeneration field.
      JournalPosition position;
      client->sync_getCurrentJournalPosition(position, mountPoint);
      // dial back to the sequence number from the query
      position.sequenceNumber = ctx->since.clock.ticks;

      // Now we can get the change journal from eden
      try {
        client->sync_getFilesChangedSince(delta, mountPoint, position);
        fileNames = std::move(delta.paths);
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
        client->sync_getCurrentJournalPosition(resultPosition, mountPoint);
        fileNames = getAllFiles();
      }
    }

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    if (info.size() != fileNames.size()) {
      throw std::runtime_error(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo, w_string::pathCat({mountPoint, name}), &resultPosition);

      if (!w_query_process_file(ctx->query, ctx, std::move(file))) {
        result = false;
        break;
      }

      ++nameIter;
      ++infoIter;
    }

    *num_walked = fileNames.size();
    return result;
  }

  bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
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
    return executeGlobBasedQuery(globStrings, query, ctx, num_walked);
  }

  bool syncToNow(std::chrono::milliseconds) override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return true;
  }

  bool executeGlobBasedQuery(
      const std::vector<std::string>& globStrings,
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const {
    auto client = getEdenClient();
    bool result = true;
    auto mountPoint = to<std::string>(ctx->root->root_path);

    std::vector<std::string> fileNames;
    client->sync_glob(fileNames, mountPoint, globStrings);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    if (info.size() != fileNames.size()) {
      throw std::runtime_error(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo, w_string::pathCat({mountPoint, name}));

      if (!w_query_process_file(ctx->query, ctx, std::move(file))) {
        result = false;
        break;
      }

      ++nameIter;
      ++infoIter;
    }

    *num_walked = fileNames.size();
    return result;
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
  bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
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
        throw std::runtime_error(
            "the eden watcher only supports depth 0 or depth -1");
      }
      // -1 depth is infinite which we can translate to a recursive
      // glob.  0 depth is direct descendant which we can translate
      // to a simple * wildcard.
      auto glob = path.depth == -1 ? "**/*" : "*";

      globStrings.emplace_back(to<std::string>(
          w_string::pathCat({rel, escapeGlobSpecialChars(path.name), glob})));
    }
    return executeGlobBasedQuery(globStrings, query, ctx, num_walked);
  }

  bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
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
      throw std::runtime_error(
          "glob_noescape is not supported for the eden watcher");
    }
    auto includedotfiles = json_is_true(
        query->query_spec.get_default("glob_includedotfiles", json_false()));
    if (includedotfiles) {
      throw std::runtime_error(
          "glob_includedotfiles is not supported for the eden watcher");
    }
    return executeGlobBasedQuery(globStrings, query, ctx, num_walked);
  }

  bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override {
    // If the query is anchored to a relative_root, use that that
    // avoid sucking down a massive list of files from eden
    std::string globPattern;
    auto rel = computeRelativePathPiece(ctx);
    if (rel.size() > 0) {
      globPattern.append(rel.data(), rel.size());
      globPattern.append("/");
    }
    globPattern.append("**");
    return executeGlobBasedQuery(
        std::vector<std::string>{globPattern}, query, ctx, num_walked);
  }

  ClockPosition getMostRecentRootNumberAndTickValue() const override {
    auto client = getEdenClient();
    JournalPosition position;
    auto mountPoint = to<std::string>(root_path_);
    client->sync_getCurrentJournalPosition(position, mountPoint);
    return ClockPosition(position.mountGeneration, position.sequenceNumber);
  }

  w_string getCurrentClockString() const override {
    return ClockPosition().toClockString();
  }

  uint32_t getLastAgeOutTickValue() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return 0;
  }

  time_t getLastAgeOutTimeStamp() const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return 0;
  }

  void ageOut(w_perf_t& sample, std::chrono::seconds minAge) override {}

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  void startThreads(const std::shared_ptr<w_root_t>& root) override {}
  void signalThreads() override {}

  const w_string& getName() const override {
    static w_string name("eden");
    return name;
  }

  std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<w_root_t>& root) override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }
};

std::shared_ptr<watchman::QueryableView> detectEden(w_root_t* root) {
  // This is mildly ghetto, but the way we figure out if the intended path
  // is on an eden mount is to ask eden to stat the root of that mount;
  // if it throws then it is not an eden mount.
  auto client = getEdenClient();

  std::vector<FileInformationOrError> info;
  static const std::vector<std::string> paths{""};
  client->sync_getFileInformation(
      info, std::string(root->root_path.data(), root->root_path.size()), paths);

  return std::make_shared<EdenView>(root);
}

} // anon namespace

static WatcherRegistry
    reg("eden", detectEden, 100 /* prefer eden above others */);
} // watchman namespace
