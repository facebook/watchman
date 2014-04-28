<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class InvalidRootTestCase extends WatchmanTestCase {
  function testInvalidRoot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $res = $this->watch("$root/invalid", false);
    $this->assertEqual(
      "unable to resolve root $root/invalid: ".
      "realpath($root/invalid) -> No such file or directory",
      $res['error']
    );
  }

}


