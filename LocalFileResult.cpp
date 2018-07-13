#include "LocalFileResult.h"
#include "ContentHash.h"
#include "watchman_error_category.h"
namespace watchman {

LocalFileResult::LocalFileResult(
    const std::shared_ptr<w_root_t>& root,
    w_string_piece path,
    w_clock_t clock)
    : root_(root),
      fullPath_(w_string::pathCat({root_->root_path, path})),
      clock_(clock) {}

void LocalFileResult::getInfo() {
  if (info_.has_value()) {
    return;
  }
  try {
    info_ = getFileInformation(fullPath_.c_str(), root_->case_sensitive);
    exists_ = true;
  } catch (const std::exception&) {
    // Treat any error as effectively deleted
    exists_ = false;
    info_ = FileInformation::makeDeletedFileInformation();
  }
}

Optional<FileInformation> LocalFileResult::stat() {
  getInfo();
  return info_;
}

w_string_piece LocalFileResult::baseName() {
  return w_string_piece(fullPath_).baseName();
}

w_string_piece LocalFileResult::dirName() {
  return w_string_piece(fullPath_).dirName();
}

Optional<bool> LocalFileResult::exists() {
  getInfo();
  return exists_;
}

watchman::Future<w_string> LocalFileResult::readLink() {
  return makeFuture(readSymbolicLink(fullPath_.c_str()));
}

Optional<w_clock_t> LocalFileResult::ctime() {
  return clock_;
}

Optional<w_clock_t> LocalFileResult::otime() {
  return clock_;
}

watchman::Future<FileResult::ContentHash> LocalFileResult::getContentSha1() {
  // TODO: find a way to reference a ContentHashCache instance
  // that will work with !InMemoryView based views.
  return makeFuture(makeResultWith([&] {
    return ContentHashCache::computeHashImmediate(fullPath_.c_str());
  }));
}

void LocalFileResult::batchFetchProperties(
    const std::vector<std::unique_ptr<FileResult>>&) {}

} // namespace watchman
