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

  function testClockSync() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);
    $clock1 = $this->watchmanCommand('clock', $root, array('sync_timeout' => 100));
    $this->assertRegex('/^c:\d+:\d+:\d+:\d+$/', $clock1['clock'],
                       "looks clocky ".json_encode($clock1));
    $clock2 = $this->watchmanCommand('clock', $root, array('sync_timeout' => 100));
    $this->assertRegex('/^c:\d+:\d+:\d+:\d+$/', $clock2['clock'],
                       "looks clocky");

    $this->assertFalse($clock1 === $clock2);
  }
}
