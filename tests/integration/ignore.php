<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class ignoreTestCase extends WatchmanTestCase {
  function testIgnoreGit() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/.git");
    mkdir("$root/.git/objects");
    mkdir("$root/.git/objects/pack");
    touch("$root/foo");

    $this->watch($root);
    // prove that we don't see pack in .git as we crawl
    $this->assertFileList($root, array(
      '.git',
      '.git/objects',
      'foo'
    ));

    // And prove that we aren't watching deeply under .git
    touch("$root/.git/objects/dontlookatme");
    $this->assertFileList($root, array(
      '.git',
      '.git/objects',
      'foo'
    ));
  }
}

