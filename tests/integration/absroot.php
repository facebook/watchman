<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class absoluteRootTestCase extends WatchmanTestCase {
  function testDot() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    try {
      $this->assertEqual(true, chdir($root), "failed to chdir $root");
      $this->assertEqual(
        $root,
        w_normalize_filename(getcwd()),
        "chdir/getcwd are consistent");

      $is_cli = $this->isUsingCLI();

      if (phutil_is_windows()) {
        $dot = '';
        $err = 'unable to resolve root : path "" must be absolute';
      } else {
        $dot = '.';
        $err = 'unable to resolve root .: path "." must be absolute';
      }

      $res = $this->watch($dot, false);
      if (!$this->isUsingCLI()) {
        $this->assertEqual(
          $err,
          idx($res, 'error')
        );
      } else {
        $this->assertEqual(
          null,
          idx($res, 'error')
        );
        $this->assertEqual(
          $root,
          idx($res, 'watch')
        );
      }
    } catch (Exception $e) {
      chdir($this->getRoot());
      throw $e;
    }
  }

  function testSlash() {
    if (phutil_is_windows()) {
      $this->assertSkipped("N/A for Windows");
    }
    $res = $this->watch('/', false);
    $this->assertEqual(
      'unable to resolve root /: cannot watch "/"',
      idx($res, 'error')
    );
  }
}
