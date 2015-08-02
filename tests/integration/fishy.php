<?php

/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class fishyTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function testFishy() {
    if (phutil_is_windows()) {
      $this->assertSkipped("simple ln -s without admin on windows");
    }
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    mkdir("$root/foo");
    touch("$root/foo/a");
    $watch = $this->watch($root);

    $base = $this->watchmanCommand('find', $root, '.');
    // This is "c:PID:1" because nothing has changed in $root yet
    $clock = $base['clock'];

    $this->suspendWatchman();
    system(
      "cd $root; ".
      "mv foo bar; ".
      "ln -s bar foo"
    );
    $this->resumeWatchman();

    $this->assertFileListUsingSince($root, $clock,
      array(
        'bar',
        'bar/a',
        'foo'
      )
    );

  }
}
