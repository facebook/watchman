#pragma once
#include "thirdparty/jansson/jansson.h"
#include "watchman.h"
#include "watchman_query.h"
#include "watchman_string.h"

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
      const std::shared_ptr<w_root_t>& root_,
      w_string_piece path,
      w_clock_t clock);
  const watchman::FileInformation& stat() const override;
  // Returns the name of the file in its containing dir
  w_string_piece baseName() const override;
  // Returns the name of the containing dir relative to the
  // VFS root
  w_string_piece dirName() override;
  // Returns true if the file currently exists
  bool exists() const override;
  // Returns the symlink target
  watchman::Future<w_string> readLink() const override;

  const w_clock_t& ctime() const override;
  const w_clock_t& otime() const override;

  // Returns the SHA-1 hash of the file contents
  watchman::Future<FileResult::ContentHash> getContentSha1() override;

 private:
  void getInfo() const;
  mutable bool needInfo_{true};
  mutable bool exists_{true};
  mutable FileInformation info_;
  std::shared_ptr<w_root_t> root_;
  w_string path_;
  w_clock_t clock_;
};

} // namespace watchman
