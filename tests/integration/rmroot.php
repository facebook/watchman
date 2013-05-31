<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
class rmrootTestCase extends WatchmanTestCase {

  function testRemoveRoot() {
    if (PHP_OS == 'Linux' && getenv('TRAVIS')) {
      $this->assertSkipped('openvz and inotify unlinks == bad time');
    }
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $top = realpath($dir->getPath());

    $root = "$top/root";
    mkdir($root);
    touch("$root/hello");

    $this->watch($root);
    $this->assertFileList($root, array('hello'));

    Filesystem::remove($root);

    $this->assertFileList($root, array());

    $watches = $this->waitForWatchman(
      array('watch-list'),
      function ($list) use ($root) {
        return !in_array($root, $list['roots']);
      }
    );
    $this->assertEqual(
      false,
      in_array($root, $watches['roots']),
      "watch deleted"
    );

    mkdir($root);
    touch("$root/hello");

    $this->assertFileList($root, array());
  }


}

