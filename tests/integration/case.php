<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class caseInsensitivityTestCase extends WatchmanTestCase {
  function testChangeCase() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    mkdir("$root/foo");
    $watch = $this->watch($root);

    $this->assertFileList($root, array('foo'));

    rename("$root/foo", "$root/FOO");

    $this->assertFileList($root, array('FOO'));

    touch("$root/FOO/bar");

    $this->assertFileList($root, array('FOO', 'FOO/bar'));

    rename("$root/FOO/bar", "$root/FOO/BAR");
    $this->assertFileList($root, array('FOO', 'FOO/BAR'));

    rename("$root/FOO", "$root/foo");
    $this->assertFileList($root, array('foo', 'foo/BAR'));

    mkdir("$root/foo/baz");
    touch("$root/foo/baz/file");
    $this->assertFileList($root,
      array('foo', 'foo/BAR', 'foo/baz', 'foo/baz/file'));

    rename("$root/foo", "$root/Foo");

    $this->assertFileList($root,
      array('Foo', 'Foo/BAR', 'Foo/baz', 'Foo/baz/file'));
  }
}
