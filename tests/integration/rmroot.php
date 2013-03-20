<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
class rmrootTestCase extends WatchmanTestCase {

  function testRemoveRoot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $top = realpath($dir->getPath());

    $root = "$top/root";
    mkdir($root);
    touch("$root/hello");

    $this->watchmanCommand('watch', $root);
    $this->assertFileList($root, array('hello'));

    Filesystem::remove($root);

    $this->assertFileList($root, array());

    $watches = $this->waitForWatchman(
      array('watch-list'),
      function ($list) {
        return count($list['roots']) == 0;
      }
    );
    $this->assertEqual(array(), $watches['roots']);

    mkdir($root);
    touch("$root/hello");

    $this->assertFileList($root, array());
  }


}

