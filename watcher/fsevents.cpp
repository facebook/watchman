/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include "watchman.h"
#include "InMemoryView.h"
#include "Pipe.h"

#if HAVE_FSEVENTS

// The FSEventStreamSetExclusionPaths API has a limit of 8 items.
// If that limit is exceeded, it will fail.
#define MAX_EXCLUSIONS size_t(8)

struct watchman_fsevent {
  w_string path;
  FSEventStreamEventFlags flags;

  watchman_fsevent(w_string&& path, FSEventStreamEventFlags flags)
      : path(std::move(path)), flags(flags) {}
};

struct fse_stream {
  FSEventStreamRef stream{nullptr};
  std::shared_ptr<w_root_t> root;
  FSEventStreamEventId last_good{0};
  FSEventStreamEventId since{0};
  bool lost_sync{false};
  bool inject_drop{false};
  bool event_id_wrapped{false};
  CFUUIDRef uuid;

  fse_stream(const std::shared_ptr<w_root_t>& root, FSEventStreamEventId since)
      : root(root), since(since) {}
  ~fse_stream();
};

struct FSEventsWatcher : public Watcher {
  watchman::Pipe fse_pipe;

  std::condition_variable fse_cond;
  watchman::Synchronized<std::deque<watchman_fsevent>, std::mutex> items_;

  struct fse_stream* stream{nullptr};
  bool attempt_resync_on_drop{false};

  explicit FSEventsWatcher(w_root_t* root);

  bool start(const std::shared_ptr<w_root_t>& root) override;

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
  void FSEventsThread(const std::shared_ptr<w_root_t>& root);
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
  auto stream = (fse_stream *)clientCallBackInfo;
  auto root = stream->root;
  std::deque<watchman_fsevent> items;
  auto watcher = watcherFromRoot(root);

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
      if ((eventFlags[i] & (kFSEventStreamEventFlagUserDropped |
                            kFSEventStreamEventFlagKernelDropped)) != 0) {
        // We don't ever need to clear lost_sync as the code below will either
        // set up a new stream instance with it cleared, or will recrawl and
        // set up a whole new state for the recrawled instance.
        stream->lost_sync = true;

        log_drop_event(root,
                       eventFlags[i] & kFSEventStreamEventFlagKernelDropped);

        if (watcher->attempt_resync_on_drop) {
        // fseventsd has a reliable journal so we can attempt to resync.
        do_resync:
          if (stream->event_id_wrapped) {
            w_log(W_LOG_ERR, "fsevents lost sync and the event_ids wrapped, so "
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
            struct fse_stream *replacement =
                fse_stream_make(root, stream->last_good, failure_reason);

            if (!replacement) {
              w_log(W_LOG_ERR,
                    "Failed to rebuild fsevent stream (%s) while trying to "
                    "resync, falling back to a regular recrawl\n",
                    failure_reason.c_str());
              // Allow the UserDropped event to propagate and trigger a recrawl
              goto propagate;
            }

            if (!FSEventStreamStart(replacement->stream)) {
              w_log(W_LOG_ERR, "FSEventStreamStart failed while trying to "
                               "resync, falling back to a regular recrawl\n");
              // Allow the UserDropped event to propagate and trigger a recrawl
              goto propagate;
            }

            w_log(W_LOG_ERR, "Lost sync, so resync from last_good event %llu\n",
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

  for (i = 0; i < numEvents; i++) {
    uint32_t len;
    const char *path = paths[i];

    if (eventFlags[i] & kFSEventStreamEventFlagHistoryDone) {
      // The docs say to ignore this event; it's just a marker informing
      // us that a resync completed.  Take this opportunity to log how
      // many events were replayed to catch up.
      w_log(W_LOG_ERR, "Historical resync completed at event id %llu (caught "
                       "up on %llu events)\n",
            eventIds[i], eventIds[i] - stream->since);
      continue;
    }

    if (eventFlags[i] & kFSEventStreamEventFlagEventIdsWrapped) {
      stream->event_id_wrapped = true;
    }

    len = strlen(path);
    while (path[len-1] == '/') {
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
    auto wlock = watcher->items_.wlock();
    std::move(items.begin(), items.end(), std::back_inserter(*wlock));
    watcher->fse_cond.notify_one();
  }
}

static void fse_pipe_callback(CFFileDescriptorRef, CFOptionFlags, void*) {
  w_log(W_LOG_DBG, "pipe signalled\n");
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
    FSEventStreamEventId since,
    w_string& failure_reason) {
  FSEventStreamContext ctx;
  CFMutableArrayRef parray = nullptr;
  CFStringRef cpath = nullptr;
  double latency;
  struct stat st;
  auto watcher = watcherFromRoot(root);

  struct fse_stream* fse_stream = new struct fse_stream(root, since);

  // Each device has an optional journal maintained by fseventsd that keeps
  // track of the change events.  The journal may not be available if the
  // filesystem was mounted read-only.  The journal has an associated UUID
  // to track the version of the data.  In some cases the journal can become
  // invalidated and it will have a new UUID generated.  This can happen
  // if the EventId rolls over.
  // We need to lookup up the UUID for the associated path and use that to
  // help decide whether we can use a value of `since` other than SinceNow.
  if (stat(root->root_path.c_str(), &st)) {
    failure_reason = w_string::printf(
        "failed to stat(%s): %s\n", root->root_path.c_str(), strerror(errno));
    goto fail;
  }

  // Obtain the UUID for the device associated with the root
  fse_stream->uuid = FSEventsCopyUUIDForDevice(st.st_dev);
  if (since != kFSEventStreamEventIdSinceNow) {
    CFUUIDBytes a, b;

    if (!fse_stream->uuid) {
      // If there is no UUID available and we want to use an event offset,
      // we fail: a nullptr UUID means that the journal is not available.
      failure_reason = w_string::printf(
          "fsevents journal is not available for dev_t=%d\n", st.st_dev);
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

  memset(&ctx, 0, sizeof(ctx));
  ctx.info = fse_stream;

  parray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
  if (!parray) {
    failure_reason = w_string("CFArrayCreateMutable failed", W_STRING_UNICODE);
    goto fail;
  }

  cpath = CFStringCreateWithBytes(
      nullptr,
      (const UInt8*)root->root_path.data(),
      root->root_path.size(),
      kCFStringEncodingUTF8,
      false);
  if (!cpath) {
    failure_reason =
        w_string("CFStringCreateWithBytes failed", W_STRING_UNICODE);
    goto fail;
  }

  CFArrayAppendValue(parray, cpath);

  latency = root->config.getDouble("fsevents_latency", 0.01),
  w_log(
      W_LOG_DBG,
      "FSEventStreamCreate for path %s with latency %f seconds\n",
      root->root_path.c_str(),
      latency);

  fse_stream->stream = FSEventStreamCreate(
      nullptr,
      fse_callback,
      &ctx,
      parray,
      since,
      latency,
      kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot |
          kFSEventStreamCreateFlagFileEvents);

  if (!fse_stream->stream) {
    failure_reason = w_string("FSEventStreamCreate failed", W_STRING_UNICODE);
    goto fail;
  }

  FSEventStreamScheduleWithRunLoop(fse_stream->stream,
      CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

#ifdef HAVE_FSEVENTSTREAMSETEXCLUSIONPATHS
  if (!root->ignore.dirs_vec.empty() &&
      root->config.getBool("_use_fsevents_exclusions", true)) {
    CFMutableArrayRef ignarray;
    size_t i, nitems = std::min(root->ignore.dirs_vec.size(), MAX_EXCLUSIONS);

    ignarray = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    if (!ignarray) {
      failure_reason =
          w_string("CFArrayCreateMutable failed", W_STRING_UNICODE);
      goto fail;
    }

    for (i = 0; i < nitems; ++i) {
      const auto& path = root->ignore.dirs_vec[i];
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
    }

    if (!FSEventStreamSetExclusionPaths(fse_stream->stream, ignarray)) {
      failure_reason =
          w_string("FSEventStreamSetExclusionPaths failed", W_STRING_UNICODE);
      CFRelease(ignarray);
      goto fail;
    }

    CFRelease(ignarray);
  }
#endif

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
  CFFileDescriptorContext fdctx;

  w_set_thread_name("fsevents %s", root->root_path.c_str());

  {
    // Block until fsevents_root_start is waiting for our initialization
    auto wlock = items_.wlock();

    attempt_resync_on_drop = root->config.getBool("fsevents_try_resync", true);

    memset(&fdctx, 0, sizeof(fdctx));
    fdctx.info = root.get();

    fdref = CFFileDescriptorCreate(
        nullptr, fse_pipe.read.fd(), true, fse_pipe_callback, &fdctx);
    CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
    {
      CFRunLoopSourceRef fdsrc;

      fdsrc = CFFileDescriptorCreateRunLoopSource(nullptr, fdref, 0);
      if (!fdsrc) {
        root->failure_reason = w_string_new_typed(
            "CFFileDescriptorCreateRunLoopSource failed", W_STRING_UNICODE);
        goto done;
      }
      CFRunLoopAddSource(CFRunLoopGetCurrent(), fdsrc, kCFRunLoopDefaultMode);
      CFRelease(fdsrc);
    }

    stream = fse_stream_make(
        root, kFSEventStreamEventIdSinceNow, root->failure_reason);
    if (!stream) {
      goto done;
    }

    if (!FSEventStreamStart(stream->stream)) {
      root->failure_reason = w_string::printf(
          "FSEventStreamStart failed, look at your log file %s for "
          "lines mentioning FSEvents and see %s#fsevents for more information\n",
          log_name,
          cfg_get_trouble_url());
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

  w_log(W_LOG_DBG, "fse_thread done\n");
}

bool FSEventsWatcher::consumeNotify(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& coll) {
  struct timeval now;
  bool recurse;
  char flags_label[128];
  std::deque<watchman_fsevent> items;

  {
    auto wlock = items_.wlock();
    std::swap(items, *wlock);
  }

  gettimeofday(&now, nullptr);

  for (auto& item : items) {
    w_expand_flags(kflags, item.flags, flags_label, sizeof(flags_label));
    w_log(
        W_LOG_DBG,
        "fsevents: got %s 0x%" PRIx32 " %s\n",
        item.path.c_str(),
        item.flags,
        flags_label);

    if (item.flags & kFSEventStreamEventFlagUserDropped) {
      root->scheduleRecrawl("kFSEventStreamEventFlagUserDropped");
      break;
    }

    if (item.flags & kFSEventStreamEventFlagKernelDropped) {
      root->scheduleRecrawl("kFSEventStreamEventFlagKernelDropped");
      break;
    }

    if (item.flags & kFSEventStreamEventFlagUnmount) {
      w_log(
          W_LOG_ERR,
          "kFSEventStreamEventFlagUnmount %s, cancel watch\n",
          item.path.c_str());
      root->cancel();
      break;
    }

    if (item.flags & kFSEventStreamEventFlagRootChanged) {
      w_log(
          W_LOG_ERR,
          "kFSEventStreamEventFlagRootChanged %s, cancel watch\n",
          item.path.c_str());
      root->cancel();
      break;
    }

    recurse = (item.flags & (kFSEventStreamEventFlagMustScanSubDirs |
                             kFSEventStreamEventFlagItemRenamed))
        ? true
        : false;

    coll->add(
        item.path,
        now,
        W_PENDING_VIA_NOTIFY | (recurse ? W_PENDING_RECURSIVE : 0));
  }

  return !items.empty();
}

FSEventsWatcher::FSEventsWatcher(w_root_t*)
    : Watcher(
          "fsevents",
          WATCHER_HAS_PER_FILE_NOTIFICATIONS | WATCHER_COALESCED_RENAME) {
}

void FSEventsWatcher::signalThreads() {
  write(fse_pipe.write.fd(), "X", 1);
}

bool FSEventsWatcher::start(const std::shared_ptr<w_root_t>& root) {
  // Spin up the fsevents processing thread; it owns a ref on the root

  auto self = std::dynamic_pointer_cast<FSEventsWatcher>(shared_from_this());
  try {
    // Acquire the mutex so thread initialization waits until we release it
    auto wlock = items_.wlock();

    std::thread thread([self, root]() {
      try {
        self->FSEventsThread(root);
      } catch (const std::exception& e) {
        watchman::log(watchman::ERR, "uncaught exception: ", e.what());
        root->cancel();
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
      w_log(
          W_LOG_ERR,
          "failed to start fsevents thread: %s\n",
          root->failure_reason.c_str());
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
  auto wlock = items_.wlock();
  fse_cond.wait_for(
      wlock.getUniqueLock(), std::chrono::milliseconds(timeoutms));
  return !wlock->empty();
}

std::unique_ptr<watchman_dir_handle> FSEventsWatcher::startWatchDir(
    const std::shared_ptr<w_root_t>&,
    struct watchman_dir*,
    struct timeval,
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

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

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
    auto wlock = watcher->items_.wlock();
    last_good = watcher->stream->last_good;
    watcher->stream->inject_drop = true;
  }

  auto resp = make_response();
  resp.set("last_good", json_integer(last_good));
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG("debug-fsevents-inject-drop", cmd_debug_fsevents_inject_drop,
          CMD_DAEMON, w_cmd_realpath_root);

#endif // HAVE_FSEVENTS

/* vim:ts=2:sw=2:et:
 */
