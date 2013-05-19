<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class SinceExprTestCase extends WatchmanTestCase {
  function testSInceExpr() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watchmanCommand('watch', $root);

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
      'expression' => array('since', $base, 'ctime'),
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
  }
}

// vim:ts=2:sw=2:et:
