<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class removeTestCase extends WatchmanTestCase {
  function testRemove() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    mkdir("$root/one");
    touch("$root/one/onefile");
    mkdir("$root/one/two");
    touch("$root/one/two/twofile");
    touch("$root/top");

    $this->watchmanCommand('watch', $root);
    $this->assertFileList($root, array(
      'one',
      'one/onefile',
      'one/two',
      'one/two/twofile',
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: remove dir one');
    Filesystem::remove("$root/one");

    $this->assertFileList($root, array(
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: touch file one');
    touch("$root/one");
    $this->assertFileList($root, array(
      'one',
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: unlink file one');
    unlink("$root/one");
    $this->assertFileList($root, array(
      'top'
    ));

    system("rm -rf $root ; mkdir -p $root/notme");

    $this->assertEqual(
      array(),
      idx($this->watchmanCommand('watch-list'), 'roots'),
      "don't believe that root/notme exists"
    );
  }
}



