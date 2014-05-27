<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class pathGeneratorTestCase extends WatchmanTestCase {
  function testPathGeneratorDot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $this->watch($root);
    $results = $this->watchmanCommand('query', $root, array(
      'path' => array('.'),
    ));

    // Assert that we didn't crash
    $this->assertEqual(array(), $results['files']);
  }

  function testPathGeneratorCase() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    mkdir("$root/foo");
    touch("$root/foo/bar");
    $this->watch($root);

    $results = $this->watchmanCommand('query', $root, array(
      'path' => array('foo'),
      'fields' => array('name'),
    ));

    $this->assertEqual(array('foo/bar'), $results['files']);

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
}
