/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <algorithm>
#include "eden/fs/service/gen-cpp2/StreamingEdenService.h"
#include "watchman.h"

#include "QueryableView.h"
#include "ThreadPool.h"

using facebook::eden::StreamingEdenServiceAsyncClient;
using facebook::eden::FileInformationOrError;
using facebook::eden::FileInformation;
using facebook::eden::JournalPosition;
using facebook::eden::FileDelta;
using facebook::eden::EdenError;

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

folly::SocketAddress getEdenServerSocketAddress() {
  folly::SocketAddress addr;
  // In the future, eden will need to provide a well-supported way to locate
  // this socket, as the "local" path component here is a FB-specific default.
  auto path = folly::to<std::string>(getenv("HOME"), "/local/.eden/socket");
  addr.setFromPath(path);
  return addr;
}

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

  const watchman::FileInformation& stat() const override {
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

  Future<FileResult::ContentHash> getContentSha1() override {
    return makeFuture<FileResult::ContentHash>(
        Result<FileResult::ContentHash>(std::make_exception_ptr(
            std::runtime_error("content hash not implemented"))));
  }

 private:
  w_string fullName_;
  watchman::FileInformation stat_;
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

class EdenView : public QueryableView {
  w_string root_path_;
  folly::EventBase subscriberEventBase_;

 public:
  explicit EdenView(w_root_t* root) {
    root_path_ = root->root_path;
  }

  void timeGenerator(w_query* /*query*/, struct w_query_ctx* ctx)
      const override {
    auto client = getEdenClient(root_path_);
    auto mountPoint = to<std::string>(ctx->root->root_path);

    FileDelta delta;
    JournalPosition resultPosition;

    if (ctx->since.is_timestamp) {
      throw QueryExecError(
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

    // Filter out any ignored files
    filterOutPaths(fileNames, ctx);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    if (info.size() != fileNames.size()) {
      throw QueryExecError(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo, w_string::pathCat({mountPoint, name}), &resultPosition);

      w_query_process_file(ctx->query, ctx, std::move(file));

      ++nameIter;
      ++infoIter;
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

  bool syncToNow(std::chrono::milliseconds) override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return true;
  }

  void executeGlobBasedQuery(
      const std::vector<std::string>& globStrings,
      w_query* /*query*/,
      struct w_query_ctx* ctx) const {
    auto client = getEdenClient(ctx->root->root_path);
    auto mountPoint = to<std::string>(ctx->root->root_path);

    std::vector<std::string> fileNames;
    client->sync_glob(fileNames, mountPoint, globStrings);

    // Filter out any ignored files
    filterOutPaths(fileNames, ctx);

    std::vector<FileInformationOrError> info;
    client->sync_getFileInformation(info, mountPoint, fileNames);

    if (info.size() != fileNames.size()) {
      throw QueryExecError(
          "info.size() didn't match fileNames.size(), should be unpossible!");
    }

    auto nameIter = fileNames.begin();
    auto infoIter = info.begin();
    while (nameIter != fileNames.end()) {
      auto& name = *nameIter;
      auto& fileInfo = *infoIter;

      auto file = make_unique<EdenFileResult>(
          fileInfo, w_string::pathCat({mountPoint, name}));

      w_query_process_file(ctx->query, ctx, std::move(file));

      ++nameIter;
      ++infoIter;
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
    auto includedotfiles = json_is_true(
        query->query_spec.get_default("glob_includedotfiles", json_false()));
    if (includedotfiles) {
      throw QueryExecError(
          "glob_includedotfiles is not supported for the eden watcher");
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

  void ageOut(w_perf_t& /*sample*/, std::chrono::seconds /*minAge*/) override {}

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& /*fileNames*/) const override {
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    return false;
  }

  SCM* getSCM() const override {
    // We're going to return an eden aware implementation when we
    // get around to hooking this up.  For now, pretend there is
    // no source control.
    return nullptr;
  }

  void startThreads(const std::shared_ptr<w_root_t>& root) override {
    auto self = shared_from_this();
    std::thread thr([self, this, root]() { subscriberThread(root); });
    thr.detach();
  }

  void signalThreads() override {
    subscriberEventBase_.terminateLoopSoon();
  }

  // This is the thread that we use to listen to the stream of
  // changes coming in from the Eden server
  void subscriberThread(std::shared_ptr<w_root_t> root) {
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
    watchman::log(watchman::ERR, __FUNCTION__, "\n");
    std::promise<void> p;
    p.set_value();
    return p.get_future();
  }
};

std::shared_ptr<watchman::QueryableView> detectEden(w_root_t* root) {
  // Watchman doesn't depend on folly, so we have to put this call here, instead
  // of watchman's main().
  static folly::once_flag reg_;
  folly::call_once(
      reg_, [] { folly::SingletonVault::singleton()->registrationComplete(); });

  auto edenRoot =
      readLink(watchman::to<std::string>(root->root_path, "/.eden/root"));
  if (w_string_piece(edenRoot) != root->root_path) {
    // We aren't at the root of the eden mount
    return nullptr;
  }

  auto client = getEdenClient(root->root_path);

  // We don't strictly need to do this, since we just verified that the root
  // matches our expectations, but it can't hurt to attempt to talk to the
  // daemon directly, just in case it is broken for some reason, or in
  // case someone is trolling us with a directory structure that looks
  // like an eden mount.
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
