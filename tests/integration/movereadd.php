<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class movereaddTestCase extends WatchmanTestCase {
  function testMoveReAdd() {
    if (PHP_OS == 'Linux' && getenv('TRAVIS')) {
      $this->assertSkipped('openvz and inotify unlinks == bad time');
    }
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    mkdir("$root/foo");
    $watch = $this->watch($root);

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo'
      ),
      array(
        'foo'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/222');
    touch("$root/foo/222");
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/222',
      ),
      array(
        'foo/222'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo/bar');
    mkdir("$root/foo/bar");

    $since = array('foo/bar');
    if (in_array($watch['watcher'], array('portfs', 'kqueue'))) {
      // the parent dir reflects as changed when we mkdir within it
      array_unshift($since, 'foo');
    }

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/222',
        'foo/bar',
      ),
      $since
    );

    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo/bar');
    w_rmdir_recursive("$root/foo/bar");
    $this->watchmanCommand('log', 'debug', 'XXX: unlink foo/222');
    unlink("$root/foo/222");
    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo');
    w_rmdir_recursive("$root/foo");

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
      ),
      array(
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo');
    mkdir("$root/foo");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/222');
    touch("$root/foo/222");

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        "foo",
        "foo/222",
      ),
      array(
        "foo",
        "foo/222",
      )
    );
  }
}
