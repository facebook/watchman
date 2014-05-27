<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class SinceExprTestCase extends WatchmanTestCase {
  function testSinceExpr() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);

    $foo_data = $this->watchmanCommand('find', $root, 'foo.c');
    $first_clock = $foo_data['clock'];

    $foo_data = $foo_data['files'][0];
    $base = $foo_data['mtime'];

    // since is GT, not GTE
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $base, 'mtime'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        array('since', $base - 1, 'mtime'),
        array('name', 'foo.c'),
      ),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    if ($this->isCaseInsensitive()) {
      $res = $this->watchmanCommand('query', $root, array(
        'expression' => array(
          'allof',
          array('since', $base - 1, 'mtime'),
          array('name', 'FOO.C'),
        ),
        'fields' => array('name'),
      ));
      $this->assertEqual(array('foo.c'), $res['files']);
    }

    // try with a clock
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $first_clock),
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);

    $target = $base + 15;
    touch("$root/foo.c", $target);

    $foo_data = $this->watchmanCommand('find', $root, 'foo.c');
    $foo_data = $foo_data['files'][0];

    // try again with a clock
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $first_clock),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', $foo_data['ctime'], 'ctime'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        array('since', $base, 'mtime'),
        array('name', 'foo.c'),
      ),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    // try with a fresh clock instance -- make sure that this only returns
    // files that exist
    unlink("$root/subdir/bar.txt");
    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('since', 'c:1:1'),
      'fields' => array('name')
    ));
    $files = $res['files'];
    sort($files);
    $this->assertEqual(array('foo.c', 'subdir'), $files);
  }
}

// vim:ts=2:sw=2:et:
