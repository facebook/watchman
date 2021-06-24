#pragma once
#include "watchman/thirdparty/jansson/jansson.h"
#include "watchman/watchman.h"
#include "watchman/watchman_string.h"
#include "watchman_query.h"

namespace watchman {

/** A FileResult that exists on the local filesystem.
 * This differs from InMemoryFileResult in that we don't maintain any
 * long-lived persistent information about the file and that the methods of
 * this instance will query the local filesystem to discover the information as
 * it is accessed.
 * We do cache the results of any filesystem operations that we may perform
 * for the duration of the lifetime of a given LocalFileResult, but that
 * information is not shared beyond that lifetime.
 * FileResult objects are typically extremely short lived, existing between
 * the point in time at which a file is matched by a query and the time
 * at which the file is rendered into the results of the query.
 */
class LocalFileResult : public FileResult {
 public:
  LocalFileResult(
      const std::shared_ptr<watchman_root>& root_,
      w_string fullPath,
      w_clock_t clock);

  // Returns stat-like information about this file.  If the file doesn't
  // exist the stat information will be largely useless (it will be zeroed
  // out), but will report itself as being a regular file.  This is fine
  // today because the only source of LocalFileResult instances today is
  // based on the list of files returned from source control, and scm
  // of today only reports files, never dirs.
  folly::Optional<watchman::FileInformation> stat() override;
  folly::Optional<struct timespec> accessedTime() override;
  folly::Optional<struct timespec> modifiedTime() override;
  folly::Optional<struct timespec> changedTime() override;
  folly::Optional<size_t> size() override;

  // Returns the name of the file in its containing dir
  w_string_piece baseName() override;
  // Returns the name of the containing dir relative to the
  // VFS root
  w_string_piece dirName() override;
  // Returns true if the file currently exists
  folly::Optional<bool> exists() override;
  // Returns the symlink target
  folly::Optional<w_string> readLink() override;

  folly::Optional<w_clock_t> ctime() override;
  folly::Optional<w_clock_t> otime() override;

  // Returns the SHA-1 hash of the file contents
  folly::Optional<FileResult::ContentHash> getContentSha1() override;

  void batchFetchProperties(
      const std::vector<std::unique_ptr<FileResult>>& files) override;

 private:
  void getInfo();
  w_string getFullPath();

  bool exists_{true};
  folly::Optional<FileInformation> info_;
  std::shared_ptr<watchman_root> root_;
  w_string fullPath_;
  w_clock_t clock_;
  folly::Optional<w_string> symlinkTarget_;
  Result<FileResult::ContentHash> contentSha1_;
};

} // namespace watchman
