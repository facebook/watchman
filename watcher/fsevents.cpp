/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "make_unique.h"
#include "watchman.h"

#if HAVE_FSEVENTS

// The FSEventStreamSetExclusionPaths API has a limit of 8 items.
// If that limit is exceeded, it will fail.
#define MAX_EXCLUSIONS size_t(8)

struct watchman_fsevent {
  struct watchman_fsevent *next;
  w_string_t *path;
  FSEventStreamEventFlags flags;
};

struct fse_stream {
  FSEventStreamRef stream;
  w_root_t *root;
  FSEventStreamEventId last_good;
  FSEventStreamEventId since;
  bool lost_sync;
  bool inject_drop;
  bool event_id_wrapped;
  CFUUIDRef uuid;
};

struct FSEventsWatcher : public Watcher {
  int fse_pipe[2]{-1, -1};

  pthread_mutex_t fse_mtx;
  pthread_cond_t fse_cond;
  pthread_t fse_thread;
  bool thread_started{false};

  struct watchman_fsevent *fse_head{nullptr}, *fse_tail{nullptr};
  struct fse_stream* stream{nullptr};
  bool attempt_resync_on_drop{false};

  FSEventsWatcher()
      : Watcher(
            "fsevents",
            WATCHER_HAS_PER_FILE_NOTIFICATIONS | WATCHER_COALESCED_RENAME) {
    pthread_mutex_init(&fse_mtx, nullptr);
    pthread_cond_init(&fse_cond, nullptr);
  }

  ~FSEventsWatcher();

  bool initNew(w_root_t* root, char** errmsg) override;
  bool start(w_root_t* root) override;

  struct watchman_dir_handle* startWatchDir(
      w_root_t* root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool consumeNotify(w_root_t* root, PendingCollection::LockedPtr& coll)
      override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
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
    w_root_t* root,
    FSEventStreamEventId since,
    w_string& failure_reason);
static void fse_stream_free(struct fse_stream *fse_stream);

/** Generate a perf event for the drop */
static void log_drop_event(w_root_t *root, bool isKernel) {
  w_perf_t sample(isKernel ? "KernelDropped" : "UserDropped");
  sample.add_root_meta(root);
  sample.finish();
  sample.force_log();
  sample.log();
}

static void fse_callback(ConstFSEventStreamRef streamRef,
   void *clientCallBackInfo,
   size_t numEvents,
   void *eventPaths,
   const FSEventStreamEventFlags eventFlags[],
   const FSEventStreamEventId eventIds[])
{
  size_t i;
  auto paths = (char**)eventPaths;
  auto stream = (fse_stream *)clientCallBackInfo;
  w_root_t *root = stream->root;
  struct watchman_fsevent *head = nullptr, *tail = nullptr, *evt;
  auto watcher = (FSEventsWatcher*)root->inner.watcher.get();

  unused_parameter(streamRef);

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
            fse_stream_free(stream);

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

    evt = (watchman_fsevent*)calloc(1, sizeof(*evt));
    if (!evt) {
      w_log(W_LOG_DBG, "FSEvents: OOM!");
      w_root_cancel(root);
      break;
    }

    evt->path = w_string_new_len_typed(path, len, W_STRING_BYTE);
    evt->flags = eventFlags[i];
    if (!stream->lost_sync) {
      stream->last_good = eventIds[i];
    }
    if (tail) {
      tail->next = evt;
    } else {
      head = evt;
    }
    tail = evt;
  }

  if (!head) {
    return;
  }

  pthread_mutex_lock(&watcher->fse_mtx);
  if (watcher->fse_tail) {
    watcher->fse_tail->next = head;
  } else {
    watcher->fse_head = head;
  }
  watcher->fse_tail = tail;
  pthread_mutex_unlock(&watcher->fse_mtx);
  pthread_cond_signal(&watcher->fse_cond);
}

static void fse_pipe_callback(CFFileDescriptorRef, CFOptionFlags, void*) {
  w_log(W_LOG_DBG, "pipe signalled\n");
  CFRunLoopStop(CFRunLoopGetCurrent());
}

static void fse_stream_free(struct fse_stream *fse_stream) {
  if (fse_stream->stream) {
    FSEventStreamStop(fse_stream->stream);
    FSEventStreamInvalidate(fse_stream->stream);
    FSEventStreamRelease(fse_stream->stream);
  }
  if (fse_stream->uuid) {
    CFRelease(fse_stream->uuid);
  }
}

static struct fse_stream* fse_stream_make(
    w_root_t* root,
    FSEventStreamEventId since,
    w_string& failure_reason) {
  FSEventStreamContext ctx;
  CFMutableArrayRef parray = nullptr;
  CFStringRef cpath = nullptr;
  double latency;
  struct fse_stream *fse_stream = (struct fse_stream*)calloc(1, sizeof(*fse_stream));
  struct stat st;
  auto watcher = (FSEventsWatcher*)root->inner.watcher.get();

  if (!fse_stream) {
    // Note that w_string_new will terminate the process on OOM
    failure_reason = w_string("OOM", W_STRING_UNICODE);
    goto fail;
  }
  fse_stream->root = root;
  fse_stream->since = since;

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
  fse_stream_free(fse_stream);
  fse_stream = nullptr;
  goto out;
}

static void *fsevents_thread(void *arg)
{
  auto root = (w_root_t *)arg;
  CFFileDescriptorRef fdref;
  CFFileDescriptorContext fdctx;
  auto watcher = (FSEventsWatcher*)root->inner.watcher.get();

  w_set_thread_name("fsevents %s", root->root_path.c_str());

  // Block until fsevents_root_start is waiting for our initialization
  pthread_mutex_lock(&watcher->fse_mtx);

  watcher->attempt_resync_on_drop =
      root->config.getBool("fsevents_try_resync", true);

  memset(&fdctx, 0, sizeof(fdctx));
  fdctx.info = root;

  fdref = CFFileDescriptorCreate(
      nullptr, watcher->fse_pipe[0], true, fse_pipe_callback, &fdctx);
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

  watcher->stream = fse_stream_make(
      root, kFSEventStreamEventIdSinceNow, root->failure_reason);
  if (!watcher->stream) {
    goto done;
  }

  if (!FSEventStreamStart(watcher->stream->stream)) {
    root->failure_reason = w_string::printf(
        "FSEventStreamStart failed, look at your log file %s for "
        "lines mentioning FSEvents and see %s#fsevents for more information\n",
        log_name,
        cfg_get_trouble_url());
    goto done;
  }

  // Signal to fsevents_root_start that we're done initializing
  pthread_cond_signal(&watcher->fse_cond);
  pthread_mutex_unlock(&watcher->fse_mtx);

  // Process the events stream until we get signalled to quit
  CFRunLoopRun();

  // Since the goto's above hold fse_mtx, we should grab it here
  pthread_mutex_lock(&watcher->fse_mtx);
done:
  if (watcher->stream) {
    fse_stream_free(watcher->stream);
  }
  if (fdref) {
    CFRelease(fdref);
  }

  // Signal to fsevents_root_start that we're done initializing in
  // the failure path.  We'll also do this after we've completed
  // the run loop in the success path; it's a spurious wakeup but
  // harmless and saves us from adding and setting a control flag
  // in each of the failure `goto` statements. fsevents_root_dtor
  // will `pthread_join` us before `state` is freed.
  pthread_cond_signal(&watcher->fse_cond);
  pthread_mutex_unlock(&watcher->fse_mtx);

  w_log(W_LOG_DBG, "fse_thread done\n");
  w_root_delref_raw(root);
  return nullptr;
}

bool FSEventsWatcher::consumeNotify(
    w_root_t* root,
    PendingCollection::LockedPtr& coll) {
  struct watchman_fsevent *head, *evt;
  int n = 0;
  struct timeval now;
  bool recurse;
  auto watcher = (FSEventsWatcher*)root->inner.watcher.get();
  char flags_label[128];

  pthread_mutex_lock(&watcher->fse_mtx);
  head = watcher->fse_head;
  watcher->fse_head = nullptr;
  watcher->fse_tail = nullptr;
  pthread_mutex_unlock(&watcher->fse_mtx);

  gettimeofday(&now, 0);

  while (head) {
    evt = head;
    head = head->next;
    n++;

    w_expand_flags(kflags, evt->flags, flags_label, sizeof(flags_label));
    w_log(W_LOG_DBG, "fsevents: got %.*s 0x%" PRIx32 " %s\n", evt->path->len,
          evt->path->buf, evt->flags, flags_label);

    if (evt->flags & kFSEventStreamEventFlagUserDropped) {
      w_root_schedule_recrawl(root, "kFSEventStreamEventFlagUserDropped");
break_out:
      w_string_delref(evt->path);
      free(evt);
      break;
    }

    if (evt->flags & kFSEventStreamEventFlagKernelDropped) {
      w_root_schedule_recrawl(root, "kFSEventStreamEventFlagKernelDropped");
      goto break_out;
    }

    if (evt->flags & kFSEventStreamEventFlagUnmount) {
      w_log(W_LOG_ERR, "kFSEventStreamEventFlagUnmount %.*s, cancel watch\n",
        evt->path->len, evt->path->buf);
      w_root_cancel(root);
      goto break_out;
    }

    if (evt->flags & kFSEventStreamEventFlagRootChanged) {
      w_log(W_LOG_ERR,
        "kFSEventStreamEventFlagRootChanged %.*s, cancel watch\n",
        evt->path->len, evt->path->buf);
      w_root_cancel(root);
      goto break_out;
    }

    recurse = (evt->flags &
                (kFSEventStreamEventFlagMustScanSubDirs|
                 kFSEventStreamEventFlagItemRenamed))
              ? true : false;

    coll->add(
        evt->path,
        now,
        W_PENDING_VIA_NOTIFY | (recurse ? W_PENDING_RECURSIVE : 0));

    w_string_delref(evt->path);
    free(evt);
  }

  return n > 0;
}

bool FSEventsWatcher::initNew(w_root_t* root, char** errmsg) {
  auto watcher = watchman::make_unique<FSEventsWatcher>();

  if (!watcher) {
    *errmsg = strdup("out of memory");
    return false;
  }

  if (pipe(watcher->fse_pipe)) {
    ignore_result(asprintf(
        errmsg,
        "watch(%s): pipe error: %s",
        root->root_path.c_str(),
        strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(watcher->fse_pipe[0]);
  w_set_cloexec(watcher->fse_pipe[1]);

  root->inner.watcher = std::move(watcher);
  return true;
}

FSEventsWatcher::~FSEventsWatcher() {
  // wait for fsevents thread to quit
  if (thread_started && !pthread_equal(fse_thread, pthread_self())) {
    void* ignore;
    pthread_join(fse_thread, &ignore);
  }

  pthread_cond_destroy(&fse_cond);
  pthread_mutex_destroy(&fse_mtx);
  if (fse_pipe[0] != -1) {
    close(fse_pipe[0]);
    fse_pipe[0] = -1;
  }
  if (fse_pipe[1] != -1) {
    close(fse_pipe[1]);
    fse_pipe[1] = -1;
  }

  while (fse_head) {
    struct watchman_fsevent* evt = fse_head;
    fse_head = evt->next;

    w_string_delref(evt->path);
    free(evt);
  }
}

void FSEventsWatcher::signalThreads() {
  write(fse_pipe[1], "X", 1);
}

bool FSEventsWatcher::start(w_root_t* root) {
  int err;

  // Spin up the fsevents processing thread; it owns a ref on the root
  w_root_addref(root);

  // Acquire the mutex so thread initialization waits until we release it
  pthread_mutex_lock(&fse_mtx);

  err = pthread_create(&fse_thread, nullptr, fsevents_thread, root);
  if (err == 0) {
    // Allow thread init to proceed; wait for its signal
    pthread_cond_wait(&fse_cond, &fse_mtx);
    pthread_mutex_unlock(&fse_mtx);

    if (root->failure_reason) {
      w_log(
          W_LOG_ERR,
          "failed to start fsevents thread: %s\n",
          root->failure_reason.c_str());
      return false;
    }

    thread_started = true;
    return true;
  }

  pthread_mutex_unlock(&fse_mtx);
  w_root_delref_raw(root);
  w_log(W_LOG_ERR, "failed to start fsevents thread: %s\n", strerror(err));
  return false;
}

bool FSEventsWatcher::waitNotify(int timeoutms) {
  struct timeval now, delta, target;
  struct timespec ts;

  if (timeoutms == 0 || fse_head) {
    return fse_head ? true : false;
  }

  // Add timeout to current time, convert to absolute timespec
  gettimeofday(&now, nullptr);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, &ts);

  pthread_mutex_lock(&fse_mtx);
  pthread_cond_timedwait(&fse_cond, &fse_mtx, &ts);
  pthread_mutex_unlock(&fse_mtx);
  return fse_head ? true : false;
}

struct watchman_dir_handle* FSEventsWatcher::startWatchDir(
    w_root_t* root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  return osdir;
}

static FSEventsWatcher watcher;
Watcher* fsevents_watcher = &watcher;

// A helper command to facilitate testing that we can successfully
// resync the stream.
static void cmd_debug_fsevents_inject_drop(
    struct watchman_client* client,
    const json_ref& args) {
  FSEventStreamEventId last_good;
  struct unlocked_watchman_root unlocked;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-fsevents-inject-drop'");
    return;
  }

  if (!resolve_root_or_err(client, args, 1, false, &unlocked)) {
    return;
  }

  if (strcmp(unlocked.root->inner.watcher->name, fsevents_watcher->name)) {
    send_error_response(client, "root is not using the fsevents watcher");
    w_root_delref(&unlocked);
    return;
  }

  auto watcher = (FSEventsWatcher*)unlocked.root->inner.watcher.get();

  if (!watcher->attempt_resync_on_drop) {
    send_error_response(client, "fsevents_try_resync is not enabled");
    w_root_delref(&unlocked);
    return;
  }

  pthread_mutex_lock(&watcher->fse_mtx);
  last_good = watcher->stream->last_good;
  watcher->stream->inject_drop = true;
  pthread_mutex_unlock(&watcher->fse_mtx);

  auto resp = make_response();
  resp.set("last_good", json_integer(last_good));
  send_and_dispose_response(client, std::move(resp));
  w_root_delref(&unlocked);
}
W_CMD_REG("debug-fsevents-inject-drop", cmd_debug_fsevents_inject_drop,
          CMD_DAEMON, w_cmd_realpath_root);

#endif // HAVE_FSEVENTS

/* vim:ts=2:sw=2:et:
 */
