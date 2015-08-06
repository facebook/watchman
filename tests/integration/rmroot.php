<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
class rmrootTestCase extends WatchmanTestCase {

  function testRemoveRoot() {
    if (PHP_OS == 'Linux' && getenv('TRAVIS')) {
      $this->assertSkipped('openvz and inotify unlinks == bad time');
    }
    $dir = new WatchmanDirectoryFixture();
    $top = $dir->getPath();

    $root = $top.DIRECTORY_SEPARATOR."root";
    mkdir($root);
    touch("$root/hello");

    $this->watch($root);
    $this->assertFileList($root, array('hello'));

    w_rmdir_recursive($root);

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

    // Really need to ensure that we mkdir, otherwise the $dir fixture
    // will throw when the scope unwinds
    $this->waitFor(
      function () use ($root) {
        return @mkdir($root);
      },
       10,
       "mkdir($root) to succeed"
     );

    touch("$root/hello");

    $this->assertFileList($root, array());
  }
}
