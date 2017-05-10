<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class SinceExprTestCase extends WatchmanTestCase {
  function testSinceExpr() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);
    $this->assertFileList($root, array(
      'foo.c',
      'subdir',
      'subdir/bar.txt'
    ));

    $foo_data = $this->watchmanCommand('find', $root, 'foo.c');
    $first_clock = $foo_data['clock'];

    $foo_data = $foo_data['files'][0];
    $base = $foo_data['mtime'];

    // since is GT, not GTE
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $base, 'mtime'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array("foo.c", "subdir", "subdir/bar.txt"),
                               $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        array('since', $base - 1, 'mtime'),
        array('name', 'foo.c'),
      ),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    if ($this->isCaseInsensitive()) {
      $res = $this->watchmanCommand('query', $root, array(
        'expression' => array(
          'allof',
          array('since', $base - 1, 'mtime'),
          array('name', 'FOO.C'),
        ),
        'fields' => array('name'),
      ));
      $this->assertEqualFileList(array('foo.c'), $res['files']);
    }

    // try with a clock
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $first_clock),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array(), $res['files']);

    $target = $base + 15;
    touch("$root/foo.c", $target);

    $foo_data = $this->watchmanCommand('find', $root, 'foo.c');
    $foo_data = $foo_data['files'][0];

    // try again with a clock
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $first_clock),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $foo_data['mtime'], 'mtime'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        array('since', $base, 'mtime'),
        array('name', 'foo.c'),
      ),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    // If using a timestamp against the oclock, ensure that
    // we're comparing in the correct order.  We need to force
    // a 2 second sleep here so that the timestamp moves forward
    // by at least 1 increment for this test to work correctly
    sleep(2);
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        array('since', time()),
        array('name', 'foo.c'),
      ),
      'fields' => array('name'),
    ));
    // Should see no changes since the now current timestamp
    $this->assertEqualFileList(array(), $res['files']);

    // try with a fresh clock instance -- make sure that this only returns
    // files that exist
    unlink("$root/subdir/bar.txt");
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', 'c:1:1'),
      'fields' => array('name')
    ));
    $files = $res['files'];
    sort($files);
    $this->assertEqualFileList(array('foo.c', 'subdir'), $files);
  }
}

// vim:ts=2:sw=2:et:
