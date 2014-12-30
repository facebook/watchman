<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

function w_rmdir_recursive($path) {
  clearstatcache();
  if (is_dir($path)) {
    $kids = @scandir($path);
    if (is_array($kids)) {
      foreach ($kids as $kid) {
        if ($kid == '.' || $kid == '..') {
          continue;
        }
        w_rmdir_recursive($path . DIRECTORY_SEPARATOR . $kid);
      }
    }
    if (is_dir($path)) {
      return @rmdir($path);
    }
  }
  if (is_file($path)) {
    return unlink($path);
  }
  return !file_exists($path);
}

// Create a temporary dir that cleans up after itself when it is
// destroyed.  The cleanup will not throw; it will make a best effort
// to wipe everything out, which is important on Windows where removes
// are queued and take some time to take effect; this can cause rmdir
// to error out if we try it immediately after removing the directory
// contents, only to succeed a fraction of a second later
class WatchmanDirectoryFixture {
  protected $path;

  public function __construct() {
    $this->path = realpath(Filesystem::createTemporaryDirectory());
  }

  public function getPath($to_file = null) {
    if ($to_file) {
      return $this->path . DIRECTORY_SEPARATOR .
        ltrim(w_normalize_filename($to_file, "\\/"));
    }
    return $this->path;
  }

  public function __destruct() {
    for ($i = 0; $i < 10; $i++) {
      if (w_rmdir_recursive($this->path)) {
        return;
      }
      usleep(200);
    }
  }
}
