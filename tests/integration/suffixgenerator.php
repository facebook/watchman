<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class SuffixGeneratorTestCase extends WatchmanTestCase {
  function testGeneratorExpr() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('true'),
      'fields' => array('name'),
      'suffix' => 'c'
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('true'),
      'fields' => array('name'),
      'suffix' => array('c','txt')
    ));
    $this->assertEqualFileList(array('foo.c', 'subdir/bar.txt'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('true'),
      'fields' => array('name'),
      'suffix' => array('c', 'txt'),
      'relative_root' => 'subdir',
    ));
    $this->assertEqualFileList(array('bar.txt'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('true'),
      'fields' => array('name'),
      'suffix' => array('a' => 'b')
    ));
    $this->assertEqual(
      'failed to parse query: \'suffix\' must be a '.
      'string or an array of strings',
      $res['error']
    );

  }
}

// vim:ts=2:sw=2:et:
