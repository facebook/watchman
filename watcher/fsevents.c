/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#if HAVE_FSEVENTS

struct watchman_fsevent {
  struct watchman_fsevent *next;
  w_string_t *path;
  FSEventStreamEventFlags flags;
};

struct fsevents_root_state {
  int fse_pipe[2];

  pthread_mutex_t fse_mtx;
  pthread_cond_t fse_cond;
  pthread_t fse_thread;

  struct watchman_fsevent *fse_head, *fse_tail;
};

static void fse_callback(ConstFSEventStreamRef streamRef,
   void *clientCallBackInfo,
   size_t numEvents,
   void *eventPaths,
   const FSEventStreamEventFlags eventFlags[],
   const FSEventStreamEventId eventIds[])
{
  size_t i;
  char **paths = eventPaths;
  w_root_t *root = clientCallBackInfo;
  char pathbuf[WATCHMAN_NAME_MAX];
  struct watchman_fsevent *head = NULL, *tail = NULL, *evt;
  struct fsevents_root_state *state = root->watch;

  unused_parameter(streamRef);
  unused_parameter(eventIds);
  unused_parameter(eventFlags);

  for (i = 0; i < numEvents; i++) {
    uint32_t len;

    len = strlen(paths[i]);
    if (len >= sizeof(pathbuf)-1) {
      w_log(W_LOG_DBG, "FSEvents: %s name is too big :-(\n", paths[i]);
      w_root_cancel(root);
      break;
    }

    strcpy(pathbuf, paths[i]);
    while (pathbuf[len-1] == '/') {
      pathbuf[len-1] = '\0';
      len--;
    }

    if (w_is_ignored(root, paths[i], len)) {
      continue;
    }

    evt = calloc(1, sizeof(*evt));
    if (!evt) {
      w_log(W_LOG_DBG, "FSEvents: OOM!");
      w_root_cancel(root);
      break;
    }

    evt->path = w_string_new(pathbuf);
    evt->flags = eventFlags[i];
    if (tail) {
      tail->next = evt;
    } else {
      head = evt;
    }
    tail = evt;

    w_log(W_LOG_DBG, "fse_thread: add %s %" PRIx32 "\n", pathbuf, evt->flags);
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
  w_root_t *root = info;

  unused_parameter(fdref);
  unused_parameter(callBackTypes);
  unused_parameter(root);

  w_log(W_LOG_DBG, "pipe signalled\n");
  CFRunLoopStop(CFRunLoopGetCurrent());
}

static void *fsevents_thread(void *arg)
{
  w_root_t *root = arg;
  FSEventStreamContext ctx;
  CFMutableArrayRef parray;
  CFStringRef cpath;
  FSEventStreamRef fs_stream = NULL;
  CFFileDescriptorContext fdctx;
  CFFileDescriptorRef fdref;
  struct fsevents_root_state *state = root->watch;
  double latency;

  w_set_thread_name("fsevents %.*s", root->root_path->len,
      root->root_path->buf);

  // Block until fsevents_root_start is waiting for our initialization
  pthread_mutex_lock(&state->fse_mtx);

  memset(&ctx, 0, sizeof(ctx));
  ctx.info = root;

  memset(&fdctx, 0, sizeof(fdctx));
  fdctx.info = root;

  fdref = CFFileDescriptorCreate(NULL, state->fse_pipe[0], true,
      fse_pipe_callback, &fdctx);
  CFFileDescriptorEnableCallBacks(fdref, kCFFileDescriptorReadCallBack);
  {
    CFRunLoopSourceRef fdsrc;

    fdsrc = CFFileDescriptorCreateRunLoopSource(NULL, fdref, 0);
    if (!fdsrc) {
      root->failure_reason = w_string_new(
          "CFFileDescriptorCreateRunLoopSource failed");
      goto done;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), fdsrc, kCFRunLoopDefaultMode);
    CFRelease(fdsrc);
  }

  parray = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  if (!parray) {
    root->failure_reason = w_string_new("CFArrayCreateMutable failed");
    goto done;
  }

  cpath = CFStringCreateWithBytes(NULL, (const UInt8*)root->root_path->buf,
      root->root_path->len, kCFStringEncodingUTF8,
      false);
  if (!cpath) {
    root->failure_reason = w_string_new("CFStringCreateWithBytes failed");
    goto done;
  }

  CFArrayAppendValue(parray, cpath);
  CFRelease(cpath);

  latency = cfg_get_double(root, "fsevents_latency", 0.01),
  w_log(W_LOG_DBG,
      "FSEventStreamCreate for path %.*s with latency %f seconds\n",
      root->root_path->len,
      root->root_path->buf,
      latency);

  fs_stream = FSEventStreamCreate(NULL, fse_callback,
      &ctx, parray, kFSEventStreamEventIdSinceNow,
      latency,
      kFSEventStreamCreateFlagNoDefer|
      kFSEventStreamCreateFlagWatchRoot|
      kFSEventStreamCreateFlagFileEvents);

  if (!fs_stream) {
    root->failure_reason = w_string_new("FSEventStreamCreate failed");
    goto done;
  }

  FSEventStreamScheduleWithRunLoop(fs_stream,
      CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
  if (!FSEventStreamStart(fs_stream)) {
    root->failure_reason = w_string_make_printf(
        "FSEventStreamStart failed, look at your log file %s for "
        "lines mentioning FSEvents and see "
        "https://facebook.github.io/watchman/docs/troubleshooting.html#"
        "fsevents for more information\n", log_name);
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
  if (fs_stream) {
    FSEventStreamStop(fs_stream);
    FSEventStreamInvalidate(fs_stream);
    FSEventStreamRelease(fs_stream);
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
  w_root_delref(root);
  return NULL;
}

static bool fsevents_root_consume_notify(watchman_global_watcher_t watcher,
    w_root_t *root)
{
  struct watchman_fsevent *head, *evt;
  int n = 0;
  struct timeval now;
  bool recurse;
  struct fsevents_root_state *state = root->watch;
  unused_parameter(watcher);

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

    recurse = (evt->flags & kFSEventStreamEventFlagMustScanSubDirs)
              ? true : false;

    w_root_add_pending(root, evt->path, recurse, now, true);

    w_string_delref(evt->path);
    free(evt);
  }

  return n > 0;
}

watchman_global_watcher_t fsevents_global_init(void) {
  return NULL;
}

void fsevents_global_dtor(watchman_global_watcher_t watcher) {
  unused_parameter(watcher);
}

bool fsevents_root_init(watchman_global_watcher_t watcher, w_root_t *root,
    char **errmsg) {
  struct fsevents_root_state *state;
  unused_parameter(watcher);

  state = calloc(1, sizeof(*state));
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

void fsevents_root_dtor(watchman_global_watcher_t watcher, w_root_t *root) {
  struct fsevents_root_state *state = root->watch;
  unused_parameter(watcher);

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

static void fsevents_root_signal_threads(watchman_global_watcher_t watcher,
    w_root_t *root) {
  struct fsevents_root_state *state = root->watch;
  unused_parameter(watcher);

  write(state->fse_pipe[1], "X", 1);
}

static bool fsevents_root_start(watchman_global_watcher_t watcher,
    w_root_t *root) {
  int err;
  struct fsevents_root_state *state = root->watch;
  unused_parameter(watcher);

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
  w_log(W_LOG_ERR, "failed to start fsevents thread: %s\n", strerror(err));
  return false;
}

static bool fsevents_root_wait_notify(watchman_global_watcher_t watcher,
    w_root_t *root, int timeoutms) {
  struct fsevents_root_state *state = root->watch;
  struct timeval now, delta, target;
  struct timespec ts;
  unused_parameter(watcher);

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

static bool fsevents_root_start_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
  return true;
}

static void fsevents_root_stop_watch_file(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(file);
}

static DIR *fsevents_root_start_watch_dir(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_dir *dir, struct timeval now,
      const char *path) {
  DIR *osdir;
  unused_parameter(watcher);

  osdir = opendir_nofollow(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, NULL);
    return NULL;
  }

  return osdir;
}

static void fsevents_root_stop_watch_dir(watchman_global_watcher_t watcher,
      w_root_t *root, struct watchman_dir *dir) {
  unused_parameter(watcher);
  unused_parameter(root);
  unused_parameter(dir);
}

static void fsevents_file_free(watchman_global_watcher_t watcher,
    struct watchman_file *file) {
  unused_parameter(watcher);
  unused_parameter(file);
}

struct watchman_ops fsevents_watcher = {
  "fsevents",
  true, // per_file_notifications
  fsevents_global_init,
  fsevents_global_dtor,
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
#endif // HAVE_FSEVENTS

/* vim:ts=2:sw=2:et:
 */
