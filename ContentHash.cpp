/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "ContentHash.h"
#include "ThreadPool.h"
#include "watchman_hash.h"
#include "watchman_stream.h"
#ifdef __APPLE__
#define COMMON_DIGEST_FOR_OPENSSL
#include "CommonCrypto/CommonDigest.h"
#elif defined(_WIN32)
#include <Wincrypt.h>
#else
#include <openssl/sha.h>
#endif
#include <string>
#include "Logging.h"
#include "FileSystem.h"
#include "watchman_scopeguard.h"

namespace watchman {

using HashValue = typename ContentHashCache::HashValue;
using Node = typename ContentHashCache::Node;

bool ContentHashCacheKey::operator==(const ContentHashCacheKey& other) const {
  return fileSize == other.fileSize && mtime.tv_sec == other.mtime.tv_sec &&
      mtime.tv_nsec == other.mtime.tv_nsec &&
      relativePath == other.relativePath;
}

std::size_t ContentHashCacheKey::hashValue() const {
  return hash_128_to_64(
      w_string_hval(relativePath),
      hash_128_to_64(fileSize, hash_128_to_64(mtime.tv_sec, mtime.tv_nsec)));
}

ContentHashCache::ContentHashCache(
    const w_string& rootPath,
    size_t maxItems,
    std::chrono::milliseconds errorTTL)
    : cache_(maxItems, errorTTL), rootPath_(rootPath) {}

Future<std::shared_ptr<const Node>> ContentHashCache::get(
    const ContentHashCacheKey& key) {
  return cache_.get(
      key, [this](const ContentHashCacheKey& k) { return computeHash(k); });
}

HashValue ContentHashCache::computeHashImmediate(
    const ContentHashCacheKey& key) const {
  HashValue result;
  uint8_t buf[8192];

  auto fullPath = w_string::pathCat({rootPath_, key.relativePath});
  auto stm = w_stm_open(fullPath.c_str(), O_RDONLY);
  if (!stm) {
    throw std::system_error(
        errno,
        std::generic_category(),
        to<std::string>("w_stm_open ", fullPath));
  }

#ifndef _WIN32
  SHA_CTX ctx;
  SHA1_Init(&ctx);

  while (true) {
    auto n = stm->read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    if (n < 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          to<std::string>("while reading from ", fullPath));
    }
    SHA1_Update(&ctx, buf, n);
  }

  SHA1_Final(result.data(), &ctx);
#else
  // Use the built-in crypt provider API on windows to avoid introducing a
  // dependency on openssl in the windows build.
  HCRYPTPROV provider{0};
  HCRYPTHASH ctx{0};

  if (!CryptAcquireContext(
          &provider,
          nullptr,
          nullptr,
          PROV_RSA_FULL,
          CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    throw std::system_error(
        GetLastError(), std::system_category(), "CryptAcquireContext");
  }
  SCOPE_EXIT {
    CryptReleaseContext(provider, 0);
  };

  if (!CryptCreateHash(provider, CALG_SHA1, 0, 0, &ctx)) {
    throw std::system_error(
        GetLastError(), std::system_category(), "CryptCreateHash");
  }
  SCOPE_EXIT {
    CryptDestroyHash(ctx);
  };

  while (true) {
    auto n = stm->read(buf, sizeof(buf));
    if (n == 0) {
      break;
    }
    if (n < 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          to<std::string>("while reading from ", fullPath));
    }

    if (!CryptHashData(ctx, buf, n, 0)) {
      throw std::system_error(
          GetLastError(), std::system_category(), "CryptHashData");
    }
  }

  DWORD size = result.size();
  if (!CryptGetHashParam(ctx, HP_HASHVAL, result.data(), &size, 0)) {
    throw std::system_error(
        GetLastError(), std::system_category(), "CryptGetHashParam HP_HASHVAL");
  }
#endif

  // Since TOCTOU is everywhere and everything, double check to make sure that
  // the file looks like we were expecting at the start.  If it isn't, then
  // we want to throw an exception and avoid associating the hash of whatever
  // state we just read with this cache key.
  auto stat = getFileInformation(fullPath.c_str());
  if (size_t(stat.size) != key.fileSize ||
      stat.mtime.tv_sec != key.mtime.tv_sec ||
      stat.mtime.tv_nsec != key.mtime.tv_nsec) {
    throw std::runtime_error(
        "metadata changed during hashing; query again to get latest status");
  }

  return result;
}

Future<HashValue> ContentHashCache::computeHash(
    const ContentHashCacheKey& key) const {
  return makeFuture(key)
      .via(&getThreadPool())
      .then([this](Result<ContentHashCacheKey>&& key) {
        return computeHashImmediate(key.value());
      });
}

const w_string& ContentHashCache::rootPath() const {
  return rootPath_;
}

CacheStats ContentHashCache::stats() const {
  return cache_.stats();
}
}
