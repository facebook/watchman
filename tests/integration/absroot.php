<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class absoluteRootTestCase extends WatchmanTestCase {
  function testDot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $this->assertEqual(true, chdir($root), "failed to chdir $root");
    $this->assertEqual($root, getcwd(), "chdir/getcwd are consistent");

    $is_cli = $this->isUsingCLI();
    $res = $this->watch('.', false);
    if (!$this->isUsingCLI()) {
      $this->assertEqual(
        'unable to resolve root .: path "." must be absolute',
        idx($res, 'error')
      );
    } else {
      $this->assertEqual(
        $root,
        idx($res, 'watch')
      );
    }
    chdir($this->getRoot());
  }

  function testSlash() {
    $res = $this->watch('/', false);
    $this->assertEqual(
      'unable to resolve root /: cannot watch "/"',
      idx($res, 'error')
    );
  }
}
