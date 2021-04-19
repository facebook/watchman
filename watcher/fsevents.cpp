/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "fsevents.h"
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <vector>
#include "InMemoryView.h"
#include "LogConfig.h"
#include "Pipe.h"

#if HAVE_FSEVENTS

namespace watchman {

// The FSEventStreamSetExclusionPaths API has a limit of 8 items.
// If that limit is exceeded, it will fail.
#define MAX_EXCLUSIONS size_t(8)

struct fse_stream {
  FSEventStreamRef stream{nullptr};
  std::shared_ptr<w_root_t> root;
  FSEventsWatcher* watcher;
  FSEventStreamEventId last_good{0};
  FSEventStreamEventId since{0};
  bool lost_sync{false};
  bool inject_drop{false};
  bool event_id_wrapped{false};
  CFUUIDRef uuid;

  fse_stream(
      const std::shared_ptr<w_root_t>& root,
      FSEventsWatcher* watcher,
      FSEventStreamEventId since)
      : root(root), watcher(watcher), since(since) {}
  ~fse_stream();
};

static const struct flag_map kflags[] = {
    {kFSEventStreamEventFlagMustScanSubDirs, "MustScanSubDirs"},
    {kFSEventStreamEventFlagUserDropped, "UserDropped"},
    {kFSEventStreamEventFlagKernelDropped, "KernelDropped"},
    {kFSEventStreamEventFlagEventIdsWrapped, "EventIdsWrapped"},
    {kFSEventStreamEventFlagHistoryDone, "HistoryDone"},
    {kFSEventStreamEventFlagRootChanged, "RootChanged"},
    {kFSEventStreamEventFlagMount, "Mount"},
    {kFSEventStreamEventFlagUnmount, "Unmount"},
    {kFSEventStreamEventFlagItemCreated, "ItemCreated"},
    {kFSEventStreamEventFlagItemRemoved, "ItemRemoved"},
    {kFSEventStreamEventFlagItemInodeMetaMod, "InodeMetaMod"},
    {kFSEventStreamEventFlagItemRenamed, "ItemRenamed"},
    {kFSEventStreamEventFlagItemModified, "ItemModified"},
    {kFSEventStreamEventFlagItemFinderInfoMod, "FinderInfoMod"},
    {kFSEventStreamEventFlagItemChangeOwner, "ItemChangeOwner"},
    {kFSEventStreamEventFlagItemXattrMod, "ItemXattrMod"},
    {kFSEventStreamEventFlagItemIsFile, "ItemIsFile"},
    {kFSEventStreamEventFlagItemIsDir, "ItemIsDir"},
    {kFSEventStreamEventFlagItemIsSymlink, "ItemIsSymlink"},
    {0, nullptr},
};

static struct fse_stream* fse_stream_make(
    const std::shared_ptr<w_root_t>& root,
    FSEventsWatcher* watcher,
    FSEventStreamEventId since,
    w_string& failure_reason);

std::shared_ptr<FSEventsWatcher> watcherFromRoot(
    const std::shared_ptr<w_root_t>& root) {
  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    return nullptr;
  }

  return std::dynamic_pointer_cast<FSEventsWatcher>(view->getWatcher());
}

/** Generate a perf event for the drop */
static void log_drop_event(
    const std::shared_ptr<w_root_t>& root,
    bool isKernel) {
  w_perf_t sample(isKernel ? "KernelDropped" : "UserDropped");
  sample.add_root_meta(root);
  sample.finish();
  sample.force_log();
  sample.log();
}

static void fse_callback(
    ConstFSEventStreamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]) {
  size_t i;
  auto paths = (char**)eventPaths;
  auto stream = (fse_stream*)clientCallBackInfo;
  auto root = stream->root;
  std::vector<watchman_fsevent> items;
  auto watcher = stream->watcher;

  if (!stream->lost_sync) {
    // This is to facilitate testing via debug-fsevents-inject-drop.
    if (stream->inject_drop) {
      stream->lost_sync = true;
      log_drop_event(root, false);
      goto do_resync;
    }

    // Pre-scan to test whether we lost sync.  The intent is to be able to skip
    // processing the events from the point at which we lost sync, so we have
    // to check this before we start allocating events for the consumer.
    for (i = 0; i < numEvents; i++) {
      if ((eventFlags[i] &
           (kFSEventStreamEventFlagUserDropped |
            kFSEventStreamEventFlagKernelDropped)) != 0) {
        // We don't ever need to clear lost_sync as the code below will either
        // set up a new stream instance with it cleared, or will recrawl and
        // set up a whole new state for the recrawled instance.
        stream->lost_sync = true;

        log_drop_event(
            root, eventFlags[i] & kFSEventStreamEventFlagKernelDropped);

        if (watcher->attempt_resync_on_drop) {
        // fseventsd has a reliable journal so we can attempt to resync.
        do_resync:
          if (stream->event_id_wrapped) {
            logf(
                ERR,
                "fsevents lost sync and the event_ids wrapped, so "
                "we have no choice but to do a full recrawl\n");
            // Allow the Dropped event to propagate and trigger a recrawl
            goto propagate;
          }

          if (watcher->stream == stream) {
            // We are the active stream for this watch which means that it
            // is safe for us to proceed with changing watcher->stream.
            // Attempt to set up a new stream to resync from the last-good
            // event.  If successful, that will replace the current stream.
            // If we fail, then we allow the UserDropped event to propagate
            // to the consumer thread which has existing logic to schedule
            // a recrawl.
            w_string failure_reason;
            struct fse_stream* replacement = fse_stream_make(
                root, watcher, stream->last_good, failure_reason);

            if (!replacement) {
              logf(
                  ERR,
                  "Failed to rebuild fsevent stream ({}) while trying to "
                  "resync, falling back to a regular recrawl\n",
                  failure_reason);
              // Allow the UserDropped event to propagate and trigger a recrawl
              goto propagate;
            }

            if (!FSEventStreamStart(replacement->stream)) {
              logf(
                  ERR,
                  "FSEventStreamStart failed while trying to "
                  "resync, falling back to a regular recrawl\n");
              // Allow the UserDropped event to propagate and trigger a recrawl
              goto propagate;
            }

            logf(
                ERR,
                "Lost sync, so resync from last_good event {}\n",
                stream->last_good);

            // mark the replacement as the winner
            watcher->stream = replacement;

            // And tear ourselves down
            delete stream;

            // And we're done.
            return;
          }
        }
        break;
      }
    }
  } else if (watcher->attempt_resync_on_drop) {
    // This stream has already lost sync and our policy is to resync
    // for ourselves.  This is most likely a spurious callback triggered
    // after we'd taken action above.  We just ignore further events
    // on this particular stream and let the other stuff kick in.
    return;
  }

propagate:

  items.reserve(numEvents);
  for (i = 0; i < numEvents; i++) {
    uint32_t len;
    const char* path = paths[i];

    if (eventFlags[i] & kFSEventStreamEventFlagHistoryDone) {
      // The docs say to ignore this event; it's just a marker informing
      // us that a resync completed.  Take this opportunity to log how
      // many events were replayed to catch up.
      logf(
          ERR,
          "Historical resync completed at event id {} (caught "
          "up on {} events)\n",
          eventIds[i],
          eventIds[i] - stream->since);
      continue;
    }

    if (eventFlags[i] & kFSEventStreamEventFlagEventIdsWrapped) {
      stream->event_id_wrapped = true;
    }

    len = strlen(path);
    while (path[len - 1] == '/') {
      len--;
    }

    if (root->ignore.isIgnored(path, len)) {
      continue;
    }

    items.emplace_back(w_string(path, len), eventFlags[i]);
    if (!stream->lost_sync) {
      stream->last_good = eventIds[i];
    }
  }

  if (!items.empty()) {
    auto wlock = watcher->items_.lock();
    wlock->push_back(std::move(items));
    watcher->fse_cond.notify_one();
  }
}

static void fse_pipe_callback(CFFileDescriptorRef, CFOptionFlags, void*) {
  logf(DBG, "pipe signalled\n");
  CFRunLoopStop(CFRunLoopGetCurrent());
}

fse_stream::~fse_stream() {
  if (stream) {
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
  }
  if (uuid) {
    CFRelease(uuid);
  }
}

static fse_stream* fse_stream_make(
    const std::shared_ptr<w_root_t>& root,
    FSEventsWatcher* watcher,
    FSEventStreamEventId since,
    w_string& failure_reason) {
  auto ctx = FSEventStreamContext();
  CFMutableArrayRef parray = nullptr;
  CFStringRef cpath = nullptr;
  double latency;
  struct stat st;
  FSEventStreamCreateFlags flags;
  w_string path;

  struct fse_stream* fse_stream = new struct fse_stream(root, watcher, since);

  // Each device has an optional journal maintained by fseventsd that keeps
  // track of the change events.  The journal may not be available if the
  // filesystem was mounted read-only.  The journal has an associated UUID
  // to track the version of the data.  In some cases the journal can become
  // invalidated and it will have a new UUID generated.  This can happen
  // if the EventId rolls over.
  // We need to lookup up the UUID for the associated path and use that to
  // help decide whether we can use a value of `since` other than SinceNow.
  if (stat(root->root_path.c_str(), &st)) {
    failure_reason = w_string::build(
        "failed to stat(",
        root->root_path,
        "): ",
        folly::errnoStr(errno),
        "\n");
    goto fail;
  }

  // Obtain the UUID for the device associated with the root
  fse_stream->uuid = FSEventsCopyUUIDForDevice(st.st_dev);
  if (since != kFSEventStreamEventIdSinceNow) {
    CFUUIDBytes a, b;

    if (!fse_stream->uuid) {
      // If there is no UUID available and we want to use an event offset,
      // we fail: a nullptr UUID means that the journal is not available.
      failure_reason = w_string::build(
          "fsevents journal is not available for dev_t=", st.st_dev, "\n");
      goto fail;
    }
    // Compare the UUID with that of the current stream
    if (!watcher->stream->uuid) {
      failure_reason = w_string(
          "fsevents journal was not available for prior stream",
          W_STRING_UNICODE);
      goto fail;
    }

    a = CFUUIDGetUUIDBytes(fse_stream->uuid);
    b = CFUUIDGetUUIDBytes(watcher->stream->uuid);

    if (memcmp(&a, &b, sizeof(a)) != 0) {
      failure_reason =
          w_string("fsevents journal UUID is different", W_STRING_UNICODE);
      goto fail;
    }
  }

  ctx.info = fse_stream;

  parray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
  if (!parray) {
    failure_reason = w_string("CFArrayCreateMutable failed", W_STRING_UNICODE);
    goto fail;
  }

  if (auto subdir = watcher->subdir) {
    path = *subdir;
  } else {
    path = root->root_path;
  }

  cpath = CFStringCreateWithBytes(
      nullptr,
      (const UInt8*)path.data(),
      path.size(),
      kCFStringEncodingUTF8,
      false);
  if (!cpath) {
    failure_reason =
        w_string("CFStringCreateWithBytes failed", W_STRING_UNICODE);
    goto fail;
  }

  CFArrayAppendValue(parray, cpath);

  latency = root->config.getDouble("fsevents_latency", 0.01),
  logf(
      DBG,
      "FSEventStreamCreate for path {} with latency {} seconds\n",
      path,
      latency);

  flags = kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot;
  if (watcher->has_file_watching) {
    flags |= kFSEventStreamCreateFlagFileEvents;
  }
  fse_stream->stream = FSEventStreamCreate(
      nullptr, fse_callback, &ctx, parray, since, latency, flags);

  if (!fse_stream->stream) {
    failure_reason = w_string("FSEventStreamCreate failed", W_STRING_UNICODE);
    goto fail;
  }

  FSEventStreamScheduleWithRunLoop(
      fse_stream->stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

  if (root->config.getBool("_use_fsevents_exclusions", true)) {
    CFMutableArrayRef ignarray;
    size_t nitems = std::min(root->ignore.dirs_vec.size(), MAX_EXCLUSIONS);
    size_t appended = 0;

    ignarray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    if (!ignarray) {
      failure_reason =
          w_string("CFArrayCreateMutable failed", W_STRING_UNICODE);
      goto fail;
    }

    for (const auto& path : root->ignore.dirs_vec) {
      if (const auto& subdir = watcher->subdir) {
        if (!w_string_startswith(path, *subdir)) {
          continue;
        }
        logf(DBG, "Adding exclusion: {} for subdir: {}\n", path, *subdir);
      }

      CFStringRef ignpath;

      ignpath = CFStringCreateWithBytes(
          nullptr,
          (const UInt8*)path.data(),
          path.size(),
          kCFStringEncodingUTF8,
          false);

      if (!ignpath) {
        failure_reason =
            w_string("CFStringCreateWithBytes failed", W_STRING_UNICODE);
        CFRelease(ignarray);
        goto fail;
      }

      CFArrayAppendValue(ignarray, ignpath);
      CFRelease(ignpath);

      appended++;
      if (appended == nitems) {
        break;
      }
    }

    if (appended != 0) {
      if (!FSEventStreamSetExclusionPaths(fse_stream->stream, ignarray)) {
        failure_reason =
            w_string("FSEventStreamSetExclusionPaths failed", W_STRING_UNICODE);
        CFRelease(ignarray);
        goto fail;
      }
    }

    CFRelease(ignarray);
  }

out:
  if (parray) {
    CFRelease(parray);
  }
  if (cpath) {
    CFRelease(cpath);
  }

  return fse_stream;

fail:
  delete fse_stream;
  fse_stream = nullptr;
  goto out;
}

void FSEventsWatcher::FSEventsThread(const std::shared_ptr<w_root_t>& root) {
  CFFileDescriptorRef fdref;
  auto fdctx = CFFileDescriptorContext();

  w_set_thread_name("fsevents ", root->root_path);

  {
    // Block until fsevents_root_start is waiting for our initialization
    auto wlock = items_.lock();

    attempt_resync_on_drop = root->config.getBool("fsevents_try_resync", true);

    fdctx.info = root.get();

    fdref = CFFileDescriptorCreate(
        nullptr, fse_pipe.read.fd(), true, fse_pipe_callback, &fdctx);
    CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
    {
      CFRunLoopSourceRef fdsrc;

      fdsrc = CFFileDescriptorCreateRunLoopSource(nullptr, fdref, 0);
      if (!fdsrc) {
        root->failure_reason = w_string(
            "CFFileDescriptorCreateRunLoopSource failed", W_STRING_UNICODE);
        goto done;
      }
      CFRunLoopAddSource(CFRunLoopGetCurrent(), fdsrc, kCFRunLoopDefaultMode);
      CFRelease(fdsrc);
    }

    stream = fse_stream_make(
        root, this, kFSEventStreamEventIdSinceNow, root->failure_reason);
    if (!stream) {
      goto done;
    }

    if (!FSEventStreamStart(stream->stream)) {
      root->failure_reason = w_string::build(
          "FSEventStreamStart failed, look at your log file ",
          log_name,
          " for lines mentioning FSEvents and see ",
          cfg_get_trouble_url(),
          "#fsevents for more information\n");
      goto done;
    }

    // Signal to fsevents_root_start that we're done initializing
    fse_cond.notify_one();
  }

  // Process the events stream until we get signalled to quit
  CFRunLoopRun();

done:
  if (stream) {
    delete stream;
  }
  if (fdref) {
    CFRelease(fdref);
  }

  logf(DBG, "fse_thread done\n");
}

namespace {
bool isRootRemoved(
    const w_string& path,
    const w_string& root_path,
    const std::optional<w_string>& subdir) {
  if (subdir) {
    return path == *subdir;
  }
  return path == root_path;
}
} // namespace

Watcher::ConsumeNotifyRet FSEventsWatcher::consumeNotify(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& coll) {
  struct timeval now;
  char flags_label[128];
  std::vector<std::vector<watchman_fsevent>> items;
  bool cancelSelf = false;

  {
    auto wlock = items_.lock();
    std::swap(items, *wlock);
  }

  gettimeofday(&now, nullptr);

  for (auto& vec : items) {
    for (auto& item : vec) {
      w_expand_flags(kflags, item.flags, flags_label, sizeof(flags_label));
      logf(
          DBG,
          "fsevents: got {} {:x} {}\n",
          item.path,
          item.flags,
          flags_label);

      if (item.flags &
          (kFSEventStreamEventFlagUserDropped |
           kFSEventStreamEventFlagKernelDropped)) {
        if (!subdir) {
          root->scheduleRecrawl(flags_label);
          break;
        } else {
          w_assert(
              item.flags & kFSEventStreamEventFlagMustScanSubDirs,
              "dropped events should specify kFSEventStreamEventFlagMustScanSubDirs");
          auto reason = fmt::format("{}: {}", *subdir, flags_label);
          root->recrawlTriggered(reason.c_str());
        }
      }

      if (item.flags & kFSEventStreamEventFlagUnmount) {
        logf(
            ERR,
            "kFSEventStreamEventFlagUnmount {}, cancel watch\n",
            item.path);
        cancelSelf = true;
        break;
      }

      if ((item.flags & kFSEventStreamEventFlagItemRemoved) &&
          isRootRemoved(item.path, root->root_path, subdir)) {
        log(ERR, "Root directory removed, cancel watch\n");
        cancelSelf = true;
        break;
      }

      if (item.flags & kFSEventStreamEventFlagRootChanged) {
        logf(
            ERR,
            "kFSEventStreamEventFlagRootChanged {}, cancel watch\n",
            item.path);
        cancelSelf = true;
        break;
      }

      if (!has_file_watching && item.path.size() < root->root_path.size()) {
        // The test_watch_del_all appear to trigger this?
        log(ERR,
            "Got an event on a directory parent to the root directory: {}?\n",
            item.path);
        continue;
      }

      int flags = W_PENDING_VIA_NOTIFY;

      if (item.flags &
          (kFSEventStreamEventFlagMustScanSubDirs |
           kFSEventStreamEventFlagItemRenamed)) {
        flags |= W_PENDING_RECURSIVE;
      }

      if (item.flags &
          (kFSEventStreamEventFlagUserDropped |
           kFSEventStreamEventFlagKernelDropped)) {
        flags |= W_PENDING_IS_DESYNCED;
      }

      coll->add(item.path, now, flags);
    }
  }

  return {!items.empty(), cancelSelf};
}

FSEventsWatcher::FSEventsWatcher(
    bool hasFileWatching,
    std::optional<w_string> dir)
    : Watcher(
          hasFileWatching ? "fsevents" : "dirfsevents",
          hasFileWatching
              ? (WATCHER_HAS_PER_FILE_NOTIFICATIONS | WATCHER_COALESCED_RENAME)
              : WATCHER_ONLY_DIRECTORY_NOTIFICATIONS),
      has_file_watching(hasFileWatching),
      subdir(std::move(dir)) {}

FSEventsWatcher::FSEventsWatcher(w_root_t* root, std::optional<w_string> dir)
    : FSEventsWatcher(root->config.getBool("fsevents_watch_files", true), dir) {
}

void FSEventsWatcher::signalThreads() {
  write(fse_pipe.write.fd(), "X", 1);
}

bool FSEventsWatcher::start(const std::shared_ptr<w_root_t>& root) {
  // Spin up the fsevents processing thread; it owns a ref on the root

  auto self = std::dynamic_pointer_cast<FSEventsWatcher>(shared_from_this());
  try {
    // Acquire the mutex so thread initialization waits until we release it
    auto wlock = items_.lock();

    std::thread thread([self, root]() {
      try {
        self->FSEventsThread(root);
      } catch (const std::exception& e) {
        watchman::log(watchman::ERR, "uncaught exception: ", e.what());
        if (!self->subdir) {
          root->cancel();
        }
      }

      // Ensure that we signal the condition variable before we
      // finish this thread.  That ensures that don't get stuck
      // waiting in FSEventsWatcher::start if something unexpected happens.
      self->fse_cond.notify_one();
    });
    // We have to detach because the readChangesThread may wind up
    // being the last thread to reference the watcher state and
    // cannot join itself.
    thread.detach();

    // Allow thread init to proceed; wait for its signal
    fse_cond.wait(wlock.getUniqueLock());

    if (root->failure_reason) {
      logf(ERR, "failed to start fsevents thread: {}\n", root->failure_reason);
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    watchman::log(
        watchman::ERR, "failed to start fsevents thread: ", e.what(), "\n");
    return false;
  }
}

bool FSEventsWatcher::waitNotify(int timeoutms) {
  auto wlock = items_.lock();
  fse_cond.wait_for(
      wlock.getUniqueLock(), std::chrono::milliseconds(timeoutms));
  return !wlock->empty();
}

std::unique_ptr<watchman_dir_handle> FSEventsWatcher::startWatchDir(
    const std::shared_ptr<w_root_t>&,
    struct watchman_dir*,
    const char* path) {
  return w_dir_open(path);
}

static RegisterWatcher<FSEventsWatcher> reg("fsevents");

// A helper command to facilitate testing that we can successfully
// resync the stream.
static void cmd_debug_fsevents_inject_drop(
    struct watchman_client* client,
    const json_ref& args) {
  FSEventStreamEventId last_good;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-fsevents-inject-drop'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto watcher = watcherFromRoot(root);
  if (!watcher) {
    send_error_response(client, "root is not using the fsevents watcher");
    return;
  }

  if (!watcher->attempt_resync_on_drop) {
    send_error_response(client, "fsevents_try_resync is not enabled");
    return;
  }

  {
    auto wlock = watcher->items_.lock();
    last_good = watcher->stream->last_good;
    watcher->stream->inject_drop = true;
  }

  auto resp = make_response();
  resp.set("last_good", json_integer(last_good));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "debug-fsevents-inject-drop",
    cmd_debug_fsevents_inject_drop,
    CMD_DAEMON,
    w_cmd_realpath_root)

} // namespace watchman

#endif // HAVE_FSEVENTS

/* vim:ts=2:sw=2:et:
 */
