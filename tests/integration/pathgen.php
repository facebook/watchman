<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class pathGeneratorTestCase extends WatchmanTestCase {
  function testPathGeneratorDot() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    $this->watch($root);
    $results = $this->watchmanCommand('query', $root, array(
      'path' => array('.'),
    ));

    // Assert that we didn't crash
    $this->assertEqual(array(), $results['files']);

    $results = $this->watchmanCommand('query', $root, array(
      'relative_root' => '.',
      'path' => array('.'),
    ));

    // Assert that we didn't crash
    $this->assertEqual(array(), $results['files']);
  }

  function testPathGeneratorCase() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    mkdir("$root/foo");
    touch("$root/foo/bar");
    $this->watch($root);

    $results = $this->watchmanCommand('query', $root, array(
      'path' => array('foo'),
      'fields' => array('name'),
    ));

    $this->assertEqualFileList(array('foo/bar'), $results['files']);

    if ($this->isCaseInsensitive()) {
      rename("$root/foo", "$root/Foo");

      $results = $this->watchmanCommand('query', $root, array(
        'path' => array('foo'), // Note case difference
        'fields' => array('name'),
      ));

      // Note: no matches.  We don't currently support case insensitive
      // matching in the path generator
      $this->assertEqual(array(), $results['files']);
    }
  }

  function testPathGeneratorRelativeRoot() {
    $dir = new WatchmanDirectoryFixture();
    $root = realpath($dir->getPath());

    touch("$root/a");
    mkdir("$root/foo");
    touch("$root/foo/bar");
    $this->watch($root);

    $results = $this->watchmanCommand('query', $root, array(
      'relative_root' => 'foo',
      'path' => array('bar'),
      'fields' => array('name'),
    ));

    $this->assertEqual(array('bar'), $results['files']);

    if ($this->isCaseInsensitive()) {
      rename("$root/foo", "$root/Foo");

      $results = $this->watchmanCommand('query', $root, array(
        'relative_root' => 'foo',
        'path' => array('bar'),
        'fields' => array('name'),
      ));

      // Note: no matches.  We don't currently support case insensitive matching
      // for relative_root
      $this->assertEqual(array(), $results['files']);
    }
  }
}
