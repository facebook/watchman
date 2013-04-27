<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class emptyExistsTestCase extends WatchmanTestCase {
  function testEmpty() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/empty");
    file_put_contents("$root/notempty", "foo");

    $this->watchmanCommand('watch', $root);
    $results = $this->watchmanCommand('query', $root, array(
      'expression' => 'empty'
    ));

    $this->assertEqual(
      'empty',
      $results['files'][0]['name']
    );

    $results = $this->watchmanCommand('query', $root, array(
      'expression' => 'exists'
    ));

    $this->assertEqual(
      'notempty',
      $results['files'][0]['name']
    );
  }

}

