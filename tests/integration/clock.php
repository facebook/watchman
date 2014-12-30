<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class clockTestCase extends WatchmanTestCase {
  function testClock() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);
    $clock = $this->watchmanCommand('clock', $root);

    $this->assertRegex('/^c:\d+:\d+:\d+:\d+$/', $clock['clock'],
                       "looks clocky");
  }
}
