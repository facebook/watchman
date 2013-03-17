<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class movereaddTestCase extends WatchmanTestCase {
  function testMoveReAdd() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/foo");
    $watch = $this->watchmanCommand('watch', $root);

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
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/222',
        'foo/bar',
      ),
      array(
        'foo/bar'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo/bar');
    rmdir("$root/foo/bar");
    $this->watchmanCommand('log', 'debug', 'XXX: unlink foo/222');
    unlink("$root/foo/222");
    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo');
    rmdir("$root/foo");

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

