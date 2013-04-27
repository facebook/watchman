<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class clockTestCase extends WatchmanTestCase {
  function testClock() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);
    $clock = $this->watchmanCommand('clock', $root);
    $find = $this->watchmanCommand('find', $root, '.');

    $this->assertEqual($find['clock'], $clock['clock']);
  }
}


