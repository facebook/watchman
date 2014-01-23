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
}


