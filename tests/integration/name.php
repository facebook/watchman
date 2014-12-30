<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class nameExprTestCase extends WatchmanTestCase {
  function testNameExpr() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('iname', 'FOO.c'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('iname', array('FOO.c', 'INVALID.txt')),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'foo.c'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', array('foo.c', 'invalid')),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'foo.c', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('foo.c'), $res['files']);

    if ($this->isCaseInsensitive()) {
      $res = $this->watchmanCommand('query', $root, array(
        'expression' => array('name', 'Foo.c', 'wholename'),
        'fields' => array('name'),
      ));
      $this->assertEqualFileList(array('foo.c'), $res['files']);
    }

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'bar.txt', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array(), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'relative_root' => 'subdir',
      'expression' => array('name', 'bar.txt', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqualFileList(array('bar.txt'), $res['files']);

    // foo.c is not in subdir directory so this shouldn't match
    $res = $this->watchmanCommand('query', $root, array(
      'relative_root' => 'subdir',
      'expression' => array('name', 'foo.c', 'wholename'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => 'name',
    ));
    $this->assertRegex(
      "/failed to parse query: Expected array for 'i?name' term/",
      $res['error']
    );

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'one', 'two', 'three'),
    ));
    $this->assertRegex(
      "/failed to parse query: Invalid number of arguments for 'i?name' term/",
      $res['error']
    );

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 2),
    ));
    $this->assertRegex(
      "/failed to parse query: Argument 2 to 'i?name' must be either ".
      "a string or an array of string/",
      $res['error']
    );

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'one', 2),
    ));
    $this->assertRegex(
      "/failed to parse query: Argument 3 to 'i?name' must be a string/",
      $res['error']
    );

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('name', 'one', 'invalid'),
    ));
    $this->assertRegex(
      "/failed to parse query: Invalid scope 'invalid' for i?name expression/",
      $res['error']
    );
  }
}
