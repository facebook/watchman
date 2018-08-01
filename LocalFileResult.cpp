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
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::FullFileInformation);
    return nullopt;
  }
  return info_;
}

Optional<size_t> LocalFileResult::size() {
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::Size);
    return nullopt;
  }
  return info_->size;
}

Optional<struct timespec> LocalFileResult::accessedTime() {
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::StatTimeStamps);
    return nullopt;
  }
  return info_->atime;
}

Optional<struct timespec> LocalFileResult::modifiedTime() {
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::StatTimeStamps);
    return nullopt;
  }
  return info_->mtime;
}

Optional<struct timespec> LocalFileResult::changedTime() {
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::StatTimeStamps);
    return nullopt;
  }
  return info_->ctime;
}

w_string_piece LocalFileResult::baseName() {
  return w_string_piece(fullPath_).baseName();
}

w_string_piece LocalFileResult::dirName() {
  return w_string_piece(fullPath_).dirName();
}

Optional<bool> LocalFileResult::exists() {
  if (!info_.has_value()) {
    accessorNeedsProperties(FileResult::Property::Exists);
    return nullopt;
  }
  return exists_;
}

Optional<w_string> LocalFileResult::readLink() {
  if (symlinkTarget_.has_value()) {
    return symlinkTarget_;
  }
  accessorNeedsProperties(FileResult::Property::SymlinkTarget);
  return nullopt;
}

Optional<w_clock_t> LocalFileResult::ctime() {
  return clock_;
}

Optional<w_clock_t> LocalFileResult::otime() {
  return clock_;
}

Optional<FileResult::ContentHash> LocalFileResult::getContentSha1() {
  if (!contentSha1_.has_value()) {
    accessorNeedsProperties(FileResult::Property::ContentSha1);
    return nullopt;
  }
  return contentSha1_;
}

void LocalFileResult::batchFetchProperties(
    const std::vector<std::unique_ptr<FileResult>>& files) {
  for (auto& f : files) {
    auto localFile = dynamic_cast<LocalFileResult*>(f.get());
    localFile->getInfo();

    if (localFile->neededProperties() & FileResult::Property::SymlinkTarget) {
      if (!localFile->info_->isSymlink()) {
        // If this file is not a symlink then we immediately yield
        // a nullptr w_string instance rather than propagating an error.
        // This behavior is relied upon by the field rendering code and
        // checked in test_symlink.py.
        localFile->symlinkTarget_ = w_string();
      } else {
        localFile->symlinkTarget_ =
            readSymbolicLink(localFile->fullPath_.c_str());
      }
    }

    if (localFile->neededProperties() & FileResult::Property::ContentSha1) {
      // TODO: find a way to reference a ContentHashCache instance
      // that will work with !InMemoryView based views.
      localFile->contentSha1_ =
          ContentHashCache::computeHashImmediate(localFile->fullPath_.c_str());
    }

    localFile->clearNeededProperties();
  }
}

} // namespace watchman
