<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class ignoreTestCase extends WatchmanTestCase {
  function testIgnoreGit() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/.git");
    touch("$root/.git/lemon");
    touch("$root/foo");

    $this->watchmanCommand('watch', $root);
    // prove that we don't see lemon in .git as we crawl
    $this->assertFileList($root, array('foo'));

    // And prove that we aren't watching .git
    touch("$root/.git/dontlookatme");
    $this->assertFileList($root, array('foo'));
  }
}

