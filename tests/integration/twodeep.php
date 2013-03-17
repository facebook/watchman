<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class twodeepTestCase extends WatchmanTestCase {
  function testTwoDeep() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array());

    $this->assertEqual(true, mkdir("$root/foo"));
    $this->assertEqual(true, mkdir("$root/foo/bar"));
    $this->assertEqual(3, file_put_contents("$root/foo/bar/111", "111"));

    $this->assertFileList($root, array(
      "foo",
      "foo/bar",
      "foo/bar/111"
    ));

    execx('rm -rf %s', "$root/foo/bar");

    $this->assertFileList($root, array(
      "foo",
    ));
  }
}



