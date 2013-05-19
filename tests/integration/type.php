<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class TypeExprTestCase extends WatchmanTestCase {
  function testTypeExpr() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watchmanCommand('watch', $root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'f'),
      'fields' => array('name'),
    ));

    $files = $res['files'];
    sort($files);
    $this->assertEqual(array('foo.c', 'subdir/bar.txt'), $files);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'd'),
      'fields' => array('name'),
    ));

    $this->assertEqual(array('subdir'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'x'),
      'fields' => array('name'),
    ));

    $this->assertEqual(
      "failed to parse query: invalid type string 'x'",
      $res['error']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => 'type',
      'fields' => array('name'),
    ));

    $this->assertEqual(
      'failed to parse query: must use ["type", "typestr"]',
      $res['error']);
  }
}

// vim:ts=2:sw=2:et:
