<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class TypeExprTestCase extends WatchmanTestCase {
  function testTypeExpr() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'f'),
      'fields' => array('name'),
    ));

    $files = $res['files'];
    sort($files);
    $this->assertEqualFileList(array('foo.c', 'subdir/bar.txt'), $files);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'd'),
      'fields' => array('name', 'type'),
    ));

    $this->assertEqual(
      $this->secondLevelSort(array(
        array('name' => 'subdir', 'type' => 'd')
      )),
      $this->secondLevelSort($res['files']));

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('type', 'f'),
      'fields' => array('name', 'type'),
    ));

    usort($res['files'], function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual(
      $this->secondLevelSort(array(
        array('name' => 'foo.c', 'type' => 'f'),
        array('name' => w_normalize_filename('subdir/bar.txt'), 'type' => 'f')
      )),
      $this->secondLevelSort($res['files']));

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
