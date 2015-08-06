<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class matchTestCase extends WatchmanTestCase {

  function testMatch() {
    $dir = new WatchmanDirectoryFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");
    mkdir("$root/foo");
    touch("$root/foo/.bar.c");
    touch("$root/foo/baz.c");

    $this->watch($root);

    $this->assertFileList(
      $root,
      array('bar.txt', 'foo.c', 'foo', 'foo/.bar.c', 'foo/baz.c'));

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          '*.c'),
      'fields' => array('name'),
      ));
    $this->assertEqualFileList(array('foo.c', 'foo/baz.c'), $res['files']);

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          '*.c',
          'wholename'),
      'fields' => array('name'),
      ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          'foo/*.c',
          'wholename'),
      'fields' => array('name'),
      ));
    $this->assertEqualFileList(array('foo/baz.c'), $res['files']);

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          '**/*.c',
          'wholename'),
        'fields' => array('name'),
      ));
    $this->assertEqualFileList(array('foo.c', 'foo/baz.c'), $res['files']);

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          '**/*.c',
          'wholename',
          array('includedotfiles' => true)),
        'fields' => array('name'),
      ));
    $this->assertEqualFileList(
      array('foo.c', 'foo/.bar.c', 'foo/baz.c'),
      $res['files']);

    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'match',
          'foo/**/*.c',
          'wholename'),
        'fields' => array('name'),
      ));
    $this->assertEqualFileList(
      array('foo/baz.c'),
      $res['files']);
  }

}
