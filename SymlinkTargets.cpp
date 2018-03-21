#include "SymlinkTargets.h"
#include <string>
#include "FileSystem.h"
#include "Logging.h"
#include "ThreadPool.h"
#include "watchman_hash.h"
#include "watchman_scopeguard.h"

namespace watchman {

using Node = typename SymlinkTargetCache::Node;

bool SymlinkTargetCacheKey::operator==(
    const SymlinkTargetCacheKey& other) const {
  return fileSize == other.fileSize && mtime.tv_sec == other.mtime.tv_sec &&
      mtime.tv_nsec == other.mtime.tv_nsec &&
      relativePath == other.relativePath;
}

std::size_t SymlinkTargetCacheKey::hashValue() const {
  return hash_128_to_64(
      w_string_hval(relativePath),
      hash_128_to_64(fileSize, hash_128_to_64(mtime.tv_sec, mtime.tv_nsec)));
}

SymlinkTargetCache::SymlinkTargetCache(
    const w_string& rootPath,
    size_t maxItems,
    std::chrono::milliseconds errorTTL)
    : cache_(maxItems, errorTTL), rootPath_(rootPath) {}

Future<std::shared_ptr<const Node>> SymlinkTargetCache::get(
    const SymlinkTargetCacheKey& key) {
  return cache_.get(
      key, [this](const SymlinkTargetCacheKey& k) { return readLink(k); });
}

w_string SymlinkTargetCache::readLinkImmediate(
    const SymlinkTargetCacheKey& key) const {
  auto fullPath = w_string::pathCat({rootPath_, key.relativePath});
  return readSymbolicLink(fullPath.c_str());
}

Future<w_string> SymlinkTargetCache::readLink(
    const SymlinkTargetCacheKey& key) const {
  return makeFuture(key)
      .via(&getThreadPool())
      .then([this](Result<SymlinkTargetCacheKey>&& key) {
        return readLinkImmediate(key.value());
      });
}

const w_string& SymlinkTargetCache::rootPath() const {
  return rootPath_;
}

CacheStats SymlinkTargetCache::stats() const {
  return cache_.stats();
}
} // namespace watchman
