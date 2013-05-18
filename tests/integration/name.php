<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class nameExprTestCase extends WatchmanTestCase {
  function testNameExpr() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watchmanCommand('watch', $root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('iname', 'FOO.c'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('iname', array('FOO.c', 'INVALID.txt')),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'foo.c'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', array('foo.c', 'invalid')),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'foo.c', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'bar.txt', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'subdir/bar.txt', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('subdir/bar.txt'), $res['files']);
  }
}


