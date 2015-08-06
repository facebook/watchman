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

// Create a temporary dir in the test harness temp tree
class WatchmanDirectoryFixture {
  protected $path;

  public function __construct() {
    $temp_dir = sys_get_temp_dir();
    for ($i = 0; $i < 100; $i++) {
      $name = $temp_dir . DIRECTORY_SEPARATOR . mt_rand();
      if (@mkdir($name)) {
        $this->path = $name;
        return;
      }
    }
    throw new Exception("failed to make a temporary dir");
  }

  public function getPath($to_file = null) {
    if ($to_file) {
      return $this->path . DIRECTORY_SEPARATOR .
        ltrim(w_normalize_filename($to_file, "\\/"));
    }
    return $this->path;
  }
}
