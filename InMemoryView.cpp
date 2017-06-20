/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "InMemoryView.h"
#include <algorithm>
#include <thread>
#include "ThreadPool.h"
#include "make_unique.h"
#include "watchman.h"
#include "watchman_scopeguard.h"

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static std::atomic<long> next_root_number{1};

namespace watchman {

InMemoryFileResult::InMemoryFileResult(
    const watchman_file* file,
    ContentHashCache& contentHashCache)
    : file_(file), contentHashCache_(contentHashCache) {}

const FileInformation& InMemoryFileResult::stat() const {
  return file_->stat;
}

w_string_piece InMemoryFileResult::baseName() const {
  return file_->getName();
}

w_string_piece InMemoryFileResult::dirName() {
  if (!dirName_) {
    dirName_ = file_->parent->getFullPath();
  }
  return dirName_;
}

bool InMemoryFileResult::exists() const {
  return file_->exists;
}

const w_clock_t& InMemoryFileResult::ctime() const {
  return file_->ctime;
}

const w_clock_t& InMemoryFileResult::otime() const {
  return file_->otime;
}

Future<w_string> InMemoryFileResult::readLink() const {
  return makeFuture(file_->symlink_target);
}

Future<FileResult::ContentHash> InMemoryFileResult::getContentSha1() {
  auto dir = dirName();
  dir.advance(contentHashCache_.rootPath().size());

  // If dirName is the root, dir.size() will now be zero
  if (dir.size() > 0) {
    // if not at the root, skip the slash character at the
    // front of dir
    dir.advance(1);
  }

  ContentHashCacheKey key{w_string::pathCat({dir, baseName()}),
                          size_t(file_->stat.size),
                          file_->stat.mtime};

  return contentHashCache_.get(key).then(
      [](Result<std::shared_ptr<const ContentHashCache::Node>>&& result)
          -> FileResult::ContentHash { return result.value()->value(); });
}

InMemoryView::view::view(const w_string& root_path)
    : root_dir(watchman::make_unique<watchman_dir>(root_path, nullptr)),
      rootNumber(next_root_number++) {}

InMemoryView::InMemoryView(w_root_t* root, std::shared_ptr<Watcher> watcher)
    : cookies_(root->cookies),
      config_(root->config),
      view_(view(root->root_path)),
      root_path(root->root_path),
      watcher_(watcher),
      contentHashCache_(
          root->root_path,
          config_.getInt("content_hash_max_items", 128 * 1024),
          std::chrono::milliseconds(
              config_.getInt("content_hash_negative_cache_ttl_ms", 2000))),
      enableContentCacheWarming_(
          config_.getBool("content_hash_warming", false)),
      maxFilesToWarmInContentCache_(
          size_t(config_.getInt("content_hash_max_warm_per_settle", 1024))),
      syncContentCacheWarming_(
          config_.getBool("content_hash_warm_wait_before_settle", false)),
      scm_(SCM::scmForPath(root->root_path)) {}

void InMemoryView::view::insertAtHeadOfFileList(struct watchman_file* file) {
  file->next = latest_file;
  if (file->next) {
    file->next->prev = &file->next;
  }
  latest_file = file;
  file->prev = &latest_file;
}

void InMemoryView::markFileChanged(
    SyncView::LockedPtr& view,
    watchman_file* file,
    const struct timeval& now) {
  if (file->exists) {
    watcher_->startWatchFile(file);
  }

  file->otime.timestamp = now.tv_sec;
  file->otime.ticks = view->mostRecentTick;

  if (view->latest_file != file) {
    // unlink from list
    file->removeFromFileList();

    // and move to the head
    view->insertAtHeadOfFileList(file);
  }
}

const watchman_dir* InMemoryView::resolveDir(
    SyncView::ConstLockedPtr& view,
    const w_string& dir_name) const {
  watchman_dir* dir;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return view->root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view->root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    w_string_t component;
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(
        &component,
        dir_component,
        sep ? (uint32_t)(sep - dir_component)
            : (uint32_t)(dir_end - dir_component),
        W_STRING_BYTE);

    auto child = dir->getChildDir(&component);
    if (!child) {
      return nullptr;
    }

    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // Does not exist
      return nullptr;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  return nullptr;
}

watchman_dir* InMemoryView::resolveDir(
    SyncView::LockedPtr& view,
    const w_string& dir_name,
    bool create) {
  watchman_dir *dir, *parent;
  const char* dir_component;
  const char* dir_end;

  if (dir_name == root_path) {
    return view->root_dir.get();
  }

  dir_component = dir_name.data();
  dir_end = dir_component + dir_name.size();

  dir = view->root_dir.get();
  dir_component += root_path.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    w_string_t component;
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_new_len_typed_stack(
        &component,
        dir_component,
        sep ? (uint32_t)(sep - dir_component)
            : (uint32_t)(dir_end - dir_component),
        W_STRING_BYTE);

    auto child = dir->getChildDir(&component);

    if (!child && !create) {
      return nullptr;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      w_string child_name(dir_component, (uint32_t)(sep - dir_component));

      auto& new_child = dir->dirs[child_name];
      new_child.reset(new watchman_dir(child_name, dir));

      child = new_child.get();
    }

    parent = dir;
    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // We need to create the dir
      break;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  w_string child_name(dir_component, (uint32_t)(dir_end - dir_component));
  auto& new_child = parent->dirs[child_name];
  new_child.reset(new watchman_dir(child_name, parent));

  dir = new_child.get();

  return dir;
}

void InMemoryView::markDirDeleted(
    SyncView::LockedPtr& view,
    struct watchman_dir* dir,
    const struct timeval& now,
    bool recursive) {
  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  for (auto& it : dir->files) {
    auto file = it.second.get();

    if (file->exists) {
      auto full_name = w_dir_path_cat_str(dir, file->getName());
      w_log(W_LOG_DBG, "mark_deleted: %s\n", full_name.c_str());
      file->exists = false;
      markFileChanged(view, file, now);
    }
  }

  if (recursive) {
    for (auto& it : dir->dirs) {
      auto child = it.second.get();

      markDirDeleted(view, child, now, true);
    }
  }
}

watchman_file* InMemoryView::getOrCreateChildFile(
    SyncView::LockedPtr& view,
    watchman_dir* dir,
    const w_string& file_name,
    const struct timeval& now) {
  // file_name is typically a baseName slice; let's use it as-is
  // to look up a child...
  auto it = dir->files.find(file_name);
  if (it != dir->files.end()) {
    return it->second.get();
  }

  // ... but take the shorter string from inside the file that
  // we create as the key.
  auto file = watchman_file::make(file_name, dir);
  auto& file_ptr = dir->files[file->getName()];
  file_ptr = std::move(file);

  file_ptr->ctime.ticks = view->mostRecentTick;
  file_ptr->ctime.timestamp = now.tv_sec;

  auto suffix = file_name.suffix();
  if (suffix) {
    auto& sufhead = view->suffixes[suffix];
    if (!sufhead) {
      // Create the list head if we don't already have one for this suffix.
      sufhead.reset(new watchman::InMemoryView::file_list_head);
    }

    file_ptr->suffix_next = sufhead->head;
    if (file_ptr->suffix_next) {
      sufhead->head->suffix_prev = &file_ptr->suffix_next;
    }
    sufhead->head = file_ptr.get();
    file_ptr->suffix_prev = &sufhead->head;
  }

  watcher_->startWatchFile(file_ptr.get());

  return file_ptr.get();
}

void InMemoryView::ageOutFile(
    std::unordered_set<w_string>& dirs_to_erase,
    watchman_file* file) {
  auto parent = file->parent;

  auto full_name = w_dir_path_cat_str(parent, file->getName());
  w_log(W_LOG_DBG, "age_out file=%s\n", full_name.c_str());

  // Revise tick for fresh instance reporting
  last_age_out_tick = std::max(last_age_out_tick, file->otime.ticks);

  // If we have a corresponding dir, we want to arrange to remove it, but only
  // after we have unlinked all of the associated file nodes.
  dirs_to_erase.insert(full_name);

  // Remove the entry from the containing file hash; this will free it.
  // We don't need to stop watching it, because we already stopped watching it
  // when we marked it as !exists.
  parent->files.erase(file->getName());
}

void InMemoryView::ageOut(w_perf_t& sample, std::chrono::seconds minAge) {
  struct watchman_file *file, *prior;
  time_t now;
  uint32_t num_aged_files = 0;
  uint32_t num_walked = 0;
  std::unordered_set<w_string> dirs_to_erase;

  time(&now);
  last_age_out_timestamp = now;
  auto view = view_.wlock();

  file = view->latest_file;
  prior = nullptr;
  while (file) {
    ++num_walked;
    if (file->exists || file->otime.timestamp + minAge.count() > now) {
      prior = file;
      file = file->next;
      continue;
    }

    ageOutFile(dirs_to_erase, file);
    num_aged_files++;

    // Go back to last good file node; we can't trust that the
    // value of file->next saved before age_out_file is a valid
    // file node as anything past that point may have also been
    // aged out along with it.
    file = prior;
  }

  for (auto& name : dirs_to_erase) {
    auto parent = resolveDir(view, name.dirName(), false);
    if (parent) {
      parent->dirs.erase(name.baseName());
    }
  }

  if (num_aged_files + dirs_to_erase.size()) {
    w_log(
        W_LOG_ERR,
        "aged %" PRIu32 " files, %" PRIu32 " dirs\n",
        num_aged_files,
        uint32_t(dirs_to_erase.size()));
  }
  sample.add_meta(
      "age_out",
      json_object({{"walked", json_integer(num_walked)},
                   {"files", json_integer(num_aged_files)},
                   {"dirs", json_integer(dirs_to_erase.size())}}));
}

void InMemoryView::timeGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;

  // Walk back in time until we hit the boundary
  auto view = view_.rlock();
  for (f = view->latest_file; f; f = f->next) {
    ctx->bumpNumWalked();
    // Note that we use <= for the time comparisons in here so that we
    // report the things that changed inclusive of the boundary presented.
    // This is especially important for clients using the coarse unix
    // timestamp as the since basis, as they would be much more
    // likely to miss out on changes if we didn't.
    if (ctx->since.is_timestamp && f->otime.timestamp <= ctx->since.timestamp) {
      break;
    }
    if (!ctx->since.is_timestamp && f->otime.ticks <= ctx->since.clock.ticks) {
      break;
    }

    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    w_query_process_file(
        query,
        ctx,
        watchman::make_unique<InMemoryFileResult>(f, contentHashCache_));
  }
}

void InMemoryView::suffixGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;

  auto view = view_.rlock();
  for (const auto& suff : query->suffixes) {
    // Head of suffix index for this suffix
    auto it = view->suffixes.find(suff);
    if (it == view->suffixes.end()) {
      continue;
    }

    // Walk and process
    for (f = it->second->head; f; f = f->suffix_next) {
      ctx->bumpNumWalked();
      if (!w_query_file_matches_relative_root(ctx, f)) {
        continue;
      }

      w_query_process_file(
          query,
          ctx,
          watchman::make_unique<InMemoryFileResult>(f, contentHashCache_));
    }
  }
}

void InMemoryView::pathGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  w_string_t* relative_root;
  struct watchman_file* f;

  if (query->relative_root) {
    relative_root = query->relative_root;
  } else {
    relative_root = root_path;
  }

  auto view = view_.rlock();

  for (const auto& path : query->paths) {
    const watchman_dir* dir;
    w_string_t* file_name;
    w_string dir_name;

    // Compose path with root
    auto full_name = w_string::pathCat({relative_root, path.name});

    // special case of root dir itself
    if (w_string_equal(root_path, full_name)) {
      // dirname on the root is outside the root, which is useless
      dir = resolveDir(view, full_name);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = full_name.dirName();
    if (!dir_name) {
      continue;
    }

    dir = resolveDir(view, dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      continue;
    }

    if (!dir->files.empty()) {
      file_name = w_string_basename(path.name);
      f = dir->getChildFile(file_name);
      w_string_delref(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !f->stat.isDir())) {
        ctx->bumpNumWalked();
        w_query_process_file(
            query,
            ctx,
            watchman::make_unique<InMemoryFileResult>(f, contentHashCache_));
        continue;
      }
    }

    // Is it a dir?
    if (dir->dirs.empty()) {
      continue;
    }

    dir = dir->getChildDir(full_name.baseName());
  is_dir:
    // We got a dir; process recursively to specified depth
    if (dir) {
      dirGenerator(query, ctx, dir, path.depth);
    }
  }
}

void InMemoryView::dirGenerator(
    w_query* query,
    struct w_query_ctx* ctx,
    const watchman_dir* dir,
    uint32_t depth) const {
  for (auto& it : dir->files) {
    auto file = it.second.get();
    ctx->bumpNumWalked();

    w_query_process_file(
        query,
        ctx,
        watchman::make_unique<InMemoryFileResult>(file, contentHashCache_));
  }

  if (depth > 0) {
    for (auto& it : dir->dirs) {
      const auto child = it.second.get();

      dirGenerator(query, ctx, child, depth - 1);
    }
  }
}

void InMemoryView::allFilesGenerator(w_query* query, struct w_query_ctx* ctx)
    const {
  struct watchman_file* f;
  auto view = view_.rlock();

  for (f = view->latest_file; f; f = f->next) {
    ctx->bumpNumWalked();
    if (!w_query_file_matches_relative_root(ctx, f)) {
      continue;
    }

    w_query_process_file(
        query,
        ctx,
        watchman::make_unique<InMemoryFileResult>(f, contentHashCache_));
  }
}

ClockPosition InMemoryView::getMostRecentRootNumberAndTickValue() const {
  auto view = view_.rlock();
  return ClockPosition(view->rootNumber, view->mostRecentTick);
}

w_string InMemoryView::getCurrentClockString() const {
  auto view = view_.rlock();
  char clockbuf[128];
  if (!clock_id_string(
          view->rootNumber, view->mostRecentTick, clockbuf, sizeof(clockbuf))) {
    throw std::runtime_error("clock string exceeded clockbuf size");
  }
  return w_string(clockbuf, W_STRING_UNICODE);
}

uint32_t InMemoryView::getLastAgeOutTickValue() const {
  return last_age_out_tick;
}

time_t InMemoryView::getLastAgeOutTimeStamp() const {
  return last_age_out_timestamp;
}

void InMemoryView::startThreads(const std::shared_ptr<w_root_t>& root) {
  // Start a thread to call into the watcher API for filesystem notifications
  auto self = std::static_pointer_cast<InMemoryView>(shared_from_this());
  w_log(W_LOG_DBG, "starting threads for %p %s\n", this, root_path.c_str());
  std::thread notifyThreadInstance([self, root]() {
    w_set_thread_name("notify %p %s", self.get(), self->root_path.c_str());
    try {
      self->notifyThread(root);
    } catch (const std::exception& e) {
      watchman::log(watchman::ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel();
    }
    watchman::log(watchman::DBG, "out of loop\n");
  });
  notifyThreadInstance.detach();

  // Wait for it to signal that the watcher has been initialized
  bool pinged = false;
  pending_.lockAndWait(std::chrono::milliseconds(-1) /* infinite */, pinged);

  // And now start the IO thread
  std::thread ioThreadInstance([self, root]() {
    w_set_thread_name("io %p %s", self.get(), self->root_path.c_str());
    try {
      self->ioThread(root);
    } catch (const std::exception& e) {
      watchman::log(watchman::ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel();
    }
    watchman::log(watchman::DBG, "out of loop\n");
  });
  ioThreadInstance.detach();
}

void InMemoryView::signalThreads() {
  w_log(W_LOG_DBG, "signalThreads! %p %s\n", this, root_path.c_str());
  stopThreads_ = true;
  watcher_->signalThreads();
  pending_.ping();
}

void InMemoryView::wakeThreads() {
  pending_.ping();
}

bool InMemoryView::doAnyOfTheseFilesExist(
    const std::vector<w_string>& fileNames) const {
  auto view = view_.rlock();
  for (auto& name : fileNames) {
    auto fullName = w_string::pathCat({root_path, name});
    const auto dir = resolveDir(view, fullName.dirName());
    if (!dir) {
      continue;
    }

    auto file = dir->getChildFile(fullName.baseName());
    if (!file) {
      continue;
    }
    if (file->exists) {
      return true;
    }
  }
  return false;
}

const std::shared_ptr<Watcher>& InMemoryView::getWatcher() const {
  return watcher_;
}

const w_string& InMemoryView::getName() const {
  return watcher_->name;
}

SCM* InMemoryView::getSCM() const {
  return scm_.get();
}

void InMemoryView::warmContentCache() {
  if (!enableContentCacheWarming_) {
    return;
  }

  watchman::log(
      watchman::DBG, "considering files for content hash cache warming\n");

  size_t n = 0;
  std::deque<Future<std::shared_ptr<const ContentHashCache::Node>>> futures;

  {
    // Walk back in time until we hit the boundary, or hit the limit
    // on the number of files we should warm up.
    auto view = view_.rlock();
    struct watchman_file* f;
    for (f = view->latest_file; f && n < maxFilesToWarmInContentCache_;
         f = f->next) {
      if (f->otime.ticks <= lastWarmedTick_) {
        watchman::log(
            watchman::DBG,
            "warmContentCache: stop because file ticks ",
            f->otime.ticks,
            " is <= lastWarmedTick_ ",
            lastWarmedTick_,
            "\n");
        break;
      }

      if (f->exists && f->stat.isFile()) {
        // Note: we could also add an expression to further constrain
        // the things we warm up here.  Let's see if we need it before
        // going ahead and adding.

        auto dirStr = f->parent->getFullPath();
        w_string_piece dir(dirStr);
        dir.advance(contentHashCache_.rootPath().size());

        // If dirName is the root, dir.size() will now be zero
        if (dir.size() > 0) {
          // if not at the root, skip the slash character at the
          // front of dir
          dir.advance(1);
        }
        ContentHashCacheKey key{w_string::pathCat({dir, f->getName()}),
                                size_t(f->stat.size),
                                f->stat.mtime};

        watchman::log(
            watchman::DBG, "warmContentCache: lookup ", key.relativePath, "\n");
        auto f = contentHashCache_.get(key);
        if (syncContentCacheWarming_) {
          futures.emplace_back(std::move(f));
        }
        ++n;
      }
    }

    lastWarmedTick_ = view->mostRecentTick;
  }

  watchman::log(
      watchman::DBG,
      "warmContentCache, lastWarmedTick_ now ",
      lastWarmedTick_,
      " scheduled ",
      n,
      " files for hashing, will wait for ",
      futures.size(),
      " lookups to finish\n");

  if (syncContentCacheWarming_) {
    // Wait for them to finish, but don't use get() because we don't
    // care about any errors that may have occurred.
    collectAll(futures.begin(), futures.end()).wait();
    watchman::log(watchman::DBG, "warmContentCache: hashing complete\n");
  }
}

void InMemoryView::debugContentHashCache(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(
        client, "wrong number of arguments for 'debug-contenthash'");
    return;
  }

  auto root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }
  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    send_error_response(client, "root is not an InMemoryView watcher");
    return;
  }

  auto stats = view->contentHashCache_.stats();
  auto resp = make_response();
  resp.set({{"cacheHit", json_integer(stats.cacheHit)},
            {"cacheShare", json_integer(stats.cacheShare)},
            {"cacheMiss", json_integer(stats.cacheMiss)},
            {"cacheEvict", json_integer(stats.cacheEvict)},
            {"cacheStore", json_integer(stats.cacheStore)},
            {"cacheLoad", json_integer(stats.cacheLoad)},
            {"cacheErase", json_integer(stats.cacheErase)},
            {"clearCount", json_integer(stats.clearCount)},
            {"size", json_integer(stats.size)}});
  send_and_dispose_response(client, std::move(resp));
}
W_CMD_REG(
    "debug-contenthash",
    InMemoryView::debugContentHashCache,
    CMD_DAEMON,
    w_cmd_realpath_root);
}
