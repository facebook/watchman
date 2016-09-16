/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#if HAVE_FSEVENTS

// The FSEventStreamSetExclusionPaths API has a limit of 8 items.
// If that limit is exceeded, it will fail.
#define MAX_EXCLUSIONS 8

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

struct fsevents_root_state {
  int fse_pipe[2];

  pthread_mutex_t fse_mtx;
  pthread_cond_t fse_cond;
  pthread_t fse_thread;

  struct watchman_fsevent *fse_head, *fse_tail;
  struct fse_stream *stream;
  bool attempt_resync_on_drop;
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
  {0, NULL},
};

static struct fse_stream *fse_stream_make(w_root_t *root,
                                          FSEventStreamEventId since,
                                          w_string_t **failure_reason);
static void fse_stream_free(struct fse_stream *fse_stream);

/** Generate a perf event for the drop */
static void log_drop_event(w_root_t *root, bool isKernel) {
  w_perf_t sample;

  w_perf_start(&sample, isKernel ? "KernelDropped" : "UserDropped");
  w_perf_add_root_meta(&sample, root);
  w_perf_finish(&sample);
  w_perf_force_log(&sample);
  w_perf_log(&sample);
  w_perf_destroy(&sample);
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
  struct watchman_fsevent *head = NULL, *tail = NULL, *evt;
  auto state = (fsevents_root_state*)root->watch;

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

        if (state->attempt_resync_on_drop) {
          // fseventsd has a reliable journal so we can attempt to resync.
do_resync:
          if (stream->event_id_wrapped) {
            w_log(W_LOG_ERR, "fsevents lost sync and the event_ids wrapped, so "
                             "we have no choice but to do a full recrawl\n");
            // Allow the Dropped event to propagate and trigger a recrawl
            goto propagate;
          }

          if (state->stream == stream) {
            // We are the active stream for this watch which means that it
            // is safe for us to proceed with changing state->stream.
            // Attempt to set up a new stream to resync from the last-good
            // event.  If successful, that will replace the current stream.
            // If we fail, then we allow the UserDropped event to propagate
            // to the consumer thread which has existing logic to schedule
            // a recrawl.
            w_string_t *failure_reason = NULL;
            struct fse_stream *replacement =
                fse_stream_make(root, stream->last_good, &failure_reason);

            if (!replacement) {
              w_log(W_LOG_ERR,
                    "Failed to rebuild fsevent stream (%.*s) while trying to "
                    "resync, falling back to a regular recrawl\n",
                    failure_reason->len, failure_reason->buf);
              w_string_delref(failure_reason);
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
            state->stream = replacement;

            // And tear ourselves down
            fse_stream_free(stream);

            // And we're done.
            return;
          }
        }
        break;
      }
    }
  } else if (state->attempt_resync_on_drop) {
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

    if (w_ignore_check(&root->ignore, path, len)) {
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

  pthread_mutex_lock(&state->fse_mtx);
  if (state->fse_tail) {
    state->fse_tail->next = head;
  } else {
    state->fse_head = head;
  }
  state->fse_tail = tail;
  pthread_mutex_unlock(&state->fse_mtx);
  pthread_cond_signal(&state->fse_cond);
}

static void fse_pipe_callback(CFFileDescriptorRef fdref,
      CFOptionFlags callBackTypes, void *info)
{
  auto root = (w_root_t*)info;

  unused_parameter(fdref);
  unused_parameter(callBackTypes);
  unused_parameter(root);

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

static struct fse_stream *fse_stream_make(w_root_t *root,
                                          FSEventStreamEventId since,
                                          w_string_t **failure_reason) {
  FSEventStreamContext ctx;
  CFMutableArrayRef parray = NULL;
  CFStringRef cpath = NULL;
  double latency;
  struct fse_stream *fse_stream = (struct fse_stream*)calloc(1, sizeof(*fse_stream));
  struct stat st;
  auto state = (fsevents_root_state*)root->watch;

  if (!fse_stream) {
    // Note that w_string_new will terminate the process on OOM
    *failure_reason = w_string_new_typed("OOM", W_STRING_UNICODE);
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
  if (stat(root->root_path->buf, &st)) {
    *failure_reason = w_string_make_printf(
        "failed to stat(%s): %s\n", root->root_path->buf, strerror(errno));
    goto fail;
  }

  // Obtain the UUID for the device associated with the root
  fse_stream->uuid = FSEventsCopyUUIDForDevice(st.st_dev);
  if (since != kFSEventStreamEventIdSinceNow) {
    CFUUIDBytes a, b;

    if (!fse_stream->uuid) {
      // If there is no UUID available and we want to use an event offset,
      // we fail: a NULL UUID means that the journal is not available.
      *failure_reason = w_string_make_printf(
          "fsevents journal is not available for dev_t=%lu\n", st.st_dev);
      goto fail;
    }
    // Compare the UUID with that of the current stream
    if (!state->stream->uuid) {
      *failure_reason =
          w_string_new_typed(
              "fsevents journal was not available for prior stream",
              W_STRING_UNICODE);
      goto fail;
    }

    a = CFUUIDGetUUIDBytes(fse_stream->uuid);
    b = CFUUIDGetUUIDBytes(state->stream->uuid);

    if (memcmp(&a, &b, sizeof(a)) != 0) {
      *failure_reason = w_string_new_typed("fsevents journal UUID is different",
          W_STRING_UNICODE);
      goto fail;
    }
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.info = fse_stream;

  parray = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  if (!parray) {
    *failure_reason = w_string_new_typed("CFArrayCreateMutable failed",
        W_STRING_UNICODE);
    goto fail;
  }

  cpath = CFStringCreateWithBytes(NULL, (const UInt8 *)root->root_path->buf,
                                  root->root_path->len, kCFStringEncodingUTF8,
                                  false);
  if (!cpath) {
    *failure_reason = w_string_new_typed("CFStringCreateWithBytes failed",
        W_STRING_UNICODE);
    goto fail;
  }

  CFArrayAppendValue(parray, cpath);

  latency = cfg_get_double(root, "fsevents_latency", 0.01),
  w_log(W_LOG_DBG,
      "FSEventStreamCreate for path %.*s with latency %f seconds\n",
      root->root_path->len,
      root->root_path->buf,
      latency);

  fse_stream->stream = FSEventStreamCreate(NULL, fse_callback,
      &ctx, parray, since,
      latency,
      kFSEventStreamCreateFlagNoDefer|
      kFSEventStreamCreateFlagWatchRoot|
      kFSEventStreamCreateFlagFileEvents);

  if (!fse_stream->stream) {
    *failure_reason = w_string_new_typed("FSEventStreamCreate failed",
        W_STRING_UNICODE);
    goto fail;
  }

  FSEventStreamScheduleWithRunLoop(fse_stream->stream,
      CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

#ifdef HAVE_FSEVENTSTREAMSETEXCLUSIONPATHS
  if (w_ht_size(root->ignore.ignore_dirs) > 0 &&
      cfg_get_bool(root, "_use_fsevents_exclusions", true)) {
    CFMutableArrayRef ignarray;
    size_t i, nitems = MIN(w_ht_size(root->ignore.ignore_dirs), MAX_EXCLUSIONS);

    ignarray = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!ignarray) {
      *failure_reason = w_string_new_typed("CFArrayCreateMutable failed",
          W_STRING_UNICODE);
      goto fail;
    }

    for (i = 0; i < nitems; ++i) {
      w_string_t *path = root->ignore.dirs_vec[i];
      CFStringRef ignpath;

      ignpath = CFStringCreateWithBytes(
          NULL, (const UInt8 *)path->buf, path->len,
          kCFStringEncodingUTF8, false);

      if (!ignpath) {
        *failure_reason = w_string_new_typed("CFStringCreateWithBytes failed",
            W_STRING_UNICODE);
        CFRelease(ignarray);
        goto fail;
      }

      CFArrayAppendValue(ignarray, ignpath);
      CFRelease(ignpath);
    }

    if (!FSEventStreamSetExclusionPaths(fse_stream->stream, ignarray)) {
      *failure_reason = w_string_new_typed(
          "FSEventStreamSetExclusionPaths failed", W_STRING_UNICODE);
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
  fse_stream = NULL;
  goto out;
}

static void *fsevents_thread(void *arg)
{
  auto root = (w_root_t *)arg;
  CFFileDescriptorRef fdref;
  CFFileDescriptorContext fdctx;
  auto state = (fsevents_root_state *)root->watch;

  w_set_thread_name("fsevents %.*s", root->root_path->len,
      root->root_path->buf);

  // Block until fsevents_root_start is waiting for our initialization
  pthread_mutex_lock(&state->fse_mtx);

  state->attempt_resync_on_drop =
      cfg_get_bool(root, "fsevents_try_resync", true);

  memset(&fdctx, 0, sizeof(fdctx));
  fdctx.info = root;

  fdref = CFFileDescriptorCreate(NULL, state->fse_pipe[0], true,
      fse_pipe_callback, &fdctx);
  CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
  {
    CFRunLoopSourceRef fdsrc;

    fdsrc = CFFileDescriptorCreateRunLoopSource(NULL, fdref, 0);
    if (!fdsrc) {
      root->failure_reason = w_string_new_typed(
          "CFFileDescriptorCreateRunLoopSource failed", W_STRING_UNICODE);
      goto done;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), fdsrc, kCFRunLoopDefaultMode);
    CFRelease(fdsrc);
  }

  state->stream = fse_stream_make(root, kFSEventStreamEventIdSinceNow,
                                  &root->failure_reason);
  if (!state->stream) {
    goto done;
  }

  if (!FSEventStreamStart(state->stream->stream)) {
    root->failure_reason = w_string_make_printf(
        "FSEventStreamStart failed, look at your log file %s for "
        "lines mentioning FSEvents and see %s#fsevents for more information\n",
        log_name, cfg_get_trouble_url());
    goto done;
  }

  // Signal to fsevents_root_start that we're done initializing
  pthread_cond_signal(&state->fse_cond);
  pthread_mutex_unlock(&state->fse_mtx);

  // Process the events stream until we get signalled to quit
  CFRunLoopRun();

  // Since the goto's above hold fse_mtx, we should grab it here
  pthread_mutex_lock(&state->fse_mtx);
done:
  if (state->stream) {
    fse_stream_free(state->stream);
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
  pthread_cond_signal(&state->fse_cond);
  pthread_mutex_unlock(&state->fse_mtx);

  w_log(W_LOG_DBG, "fse_thread done\n");
  w_root_delref_raw(root);
  return NULL;
}

static bool fsevents_root_consume_notify(w_root_t *root,
    struct watchman_pending_collection *coll)
{
  struct watchman_fsevent *head, *evt;
  int n = 0;
  struct timeval now;
  bool recurse;
  auto state = (fsevents_root_state *)root->watch;
  char flags_label[128];

  pthread_mutex_lock(&state->fse_mtx);
  head = state->fse_head;
  state->fse_head = NULL;
  state->fse_tail = NULL;
  pthread_mutex_unlock(&state->fse_mtx);

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

    w_pending_coll_add(coll, evt->path, now,
        W_PENDING_VIA_NOTIFY | (recurse ? W_PENDING_RECURSIVE : 0));

    w_string_delref(evt->path);
    free(evt);
  }

  return n > 0;
}

bool fsevents_root_init(w_root_t *root, char **errmsg) {
  struct fsevents_root_state *state;

  state = (fsevents_root_state*)calloc(1, sizeof(*state));
  if (!state) {
    *errmsg = strdup("out of memory");
    return false;
  }
  root->watch = state;

  if (pipe(state->fse_pipe)) {
    ignore_result(asprintf(errmsg, "watch(%.*s): pipe error: %s",
        root->root_path->len, root->root_path->buf, strerror(errno)));
    w_log(W_LOG_ERR, "%s\n", *errmsg);
    return false;
  }
  w_set_cloexec(state->fse_pipe[0]);
  w_set_cloexec(state->fse_pipe[1]);
  pthread_mutex_init(&state->fse_mtx, NULL);
  pthread_cond_init(&state->fse_cond, NULL);

  return true;
}

void fsevents_root_dtor(w_root_t *root) {
  auto state = (fsevents_root_state *)root->watch;

  if (!state) {
    return;
  }

  // wait for fsevents thread to quit
  if (!pthread_equal(state->fse_thread, pthread_self())) {
    void *ignore;
    pthread_join(state->fse_thread, &ignore);
  }

  pthread_cond_destroy(&state->fse_cond);
  pthread_mutex_destroy(&state->fse_mtx);
  close(state->fse_pipe[0]);
  close(state->fse_pipe[1]);

  while (state->fse_head) {
    struct watchman_fsevent *evt = state->fse_head;
    state->fse_head = evt->next;

    w_string_delref(evt->path);
    free(evt);
  }

  free(state);
  root->watch = NULL;
}

static void fsevents_root_signal_threads(w_root_t *root) {
  auto state = (fsevents_root_state *)root->watch;

  write(state->fse_pipe[1], "X", 1);
}

static bool fsevents_root_start(w_root_t *root) {
  int err;
  auto state = (fsevents_root_state *)root->watch;

  // Spin up the fsevents processing thread; it owns a ref on the root
  w_root_addref(root);

  // Acquire the mutex so thread initialization waits until we release it
  pthread_mutex_lock(&state->fse_mtx);

  err = pthread_create(&state->fse_thread, NULL, fsevents_thread, root);
  if (err == 0) {
    // Allow thread init to proceed; wait for its signal
    pthread_cond_wait(&state->fse_cond, &state->fse_mtx);
    pthread_mutex_unlock(&state->fse_mtx);

    if (root->failure_reason) {
      w_log(W_LOG_ERR, "failed to start fsevents thread: %.*s\n",
          root->failure_reason->len, root->failure_reason->buf);
      return false;
    }
    return true;
  }

  pthread_mutex_unlock(&state->fse_mtx);
  w_root_delref_raw(root);
  w_log(W_LOG_ERR, "failed to start fsevents thread: %s\n", strerror(err));
  return false;
}

static bool fsevents_root_wait_notify(w_root_t *root, int timeoutms) {
  auto state = (fsevents_root_state *)root->watch;
  struct timeval now, delta, target;
  struct timespec ts;

  if (timeoutms == 0 || state->fse_head) {
    return state->fse_head ? true : false;
  }

  // Add timeout to current time, convert to absolute timespec
  gettimeofday(&now, NULL);
  delta.tv_sec = timeoutms / 1000;
  delta.tv_usec = (timeoutms - (delta.tv_sec * 1000)) * 1000;
  w_timeval_add(now, delta, &target);
  w_timeval_to_timespec(target, &ts);

  pthread_mutex_lock(&state->fse_mtx);
  pthread_cond_timedwait(&state->fse_cond, &state->fse_mtx, &ts);
  pthread_mutex_unlock(&state->fse_mtx);
  return state->fse_head ? true : false;
}

static bool
fsevents_root_start_watch_file(struct write_locked_watchman_root *lock,
                               struct watchman_file *file) {
  unused_parameter(lock);
  unused_parameter(file);
  return true;
}

static void
fsevents_root_stop_watch_file(struct write_locked_watchman_root *lock,
                              struct watchman_file *file) {
  unused_parameter(lock);
  unused_parameter(file);
}

static struct watchman_dir_handle *
fsevents_root_start_watch_dir(struct write_locked_watchman_root *lock,
                              struct watchman_dir *dir, struct timeval now,
                              const char *path) {
  struct watchman_dir_handle *osdir;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(lock, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  return osdir;
}

static void
fsevents_root_stop_watch_dir(struct write_locked_watchman_root *lock,
                             struct watchman_dir *dir) {
  unused_parameter(lock);
  unused_parameter(dir);
}

static void fsevents_file_free(struct watchman_file *file) {
  unused_parameter(file);
}

struct watchman_ops fsevents_watcher = {
  "fsevents",
  WATCHER_HAS_PER_FILE_NOTIFICATIONS|
    WATCHER_COALESCED_RENAME,
  fsevents_root_init,
  fsevents_root_start,
  fsevents_root_dtor,
  fsevents_root_start_watch_file,
  fsevents_root_stop_watch_file,
  fsevents_root_start_watch_dir,
  fsevents_root_stop_watch_dir,
  fsevents_root_signal_threads,
  fsevents_root_consume_notify,
  fsevents_root_wait_notify,
  fsevents_file_free
};

// A helper command to facilitate testing that we can successfully
// resync the stream.
static void cmd_debug_fsevents_inject_drop(struct watchman_client *client,
                                           json_t *args) {
  json_t *resp;
  FSEventStreamEventId last_good;
  struct fsevents_root_state *state;
  struct write_locked_watchman_root lock;
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

  if (unlocked.root->watcher_ops != &fsevents_watcher) {
    send_error_response(client, "root is not using the fsevents watcher");
    w_root_delref(&unlocked);
    return;
  }

  w_root_lock(&unlocked, "debug-fsevents-inject-drop", &lock);
  state = (fsevents_root_state*)lock.root->watch;

  if (!state->attempt_resync_on_drop) {
    w_root_unlock(&lock, &unlocked);
    send_error_response(client, "fsevents_try_resync is not enabled");
    w_root_delref(&unlocked);
    return;
  }

  pthread_mutex_lock(&state->fse_mtx);
  last_good = state->stream->last_good;
  state->stream->inject_drop = true;
  pthread_mutex_unlock(&state->fse_mtx);

  w_root_unlock(&lock, &unlocked);

  resp = make_response();
  set_prop(resp, "last_good", json_integer(last_good));
  send_and_dispose_response(client, resp);
  w_root_delref(&unlocked);
}
W_CMD_REG("debug-fsevents-inject-drop", cmd_debug_fsevents_inject_drop,
          CMD_DAEMON, w_cmd_realpath_root);

#endif // HAVE_FSEVENTS

/* vim:ts=2:sw=2:et:
 */
