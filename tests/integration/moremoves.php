<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class moremovesTestCase extends WatchmanTestCase {
  function testMoreMoves() {
    if (phutil_is_windows()) {
      $this->assertSkipped("no unix userland on windows");
    }
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);
    $base = $this->watchmanCommand('find', $root, '.');
    // This is "c:PID:1" because nothing has changed in $root yet
    $clock = $base['clock'];

    $this->suspendWatchman();
    system(
      "cd $root; touch a; mkdir d1 d2 ; ".
      "mv d1 d2 ; mv d2/d1 . ; mv a d1"
    );
    $this->resumeWatchman();

    $this->assertFileListUsingSince($root, $clock,
      array(
        'd1',
        'd1/a',
        'd2'
      )
    );
  }

  function testEvenMoreMoves() {
    if (phutil_is_windows()) {
      $this->assertSkipped("no unix userland on windows");
    }
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);
    $base = $this->watchmanCommand('find', $root, '.');
    // This is "c:PID:1" because nothing has changed in $root yet
    $clock = $base['clock'];

    // TODO: this should work even if Watchman is suspended. Investigate failure
    // on Travis.
    system(
      "cd $root; ".
      "mkdir d1 d2; ".
      "touch d1/a; ".
      "mkdir d3; ".
      "mv d1 d2 d3; ".
      "mv d3/* .; ".
      "mv d1 d2 d3; ".
      "mv d3/* .; ".
      "mv d1/a d2; "
    );

    $this->assertFileListUsingSince($root, $clock,
      array(
        'd1',
        'd2',
        'd2/a',
        'd3'
      )
    );
  }
}
