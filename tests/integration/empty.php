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

    $exists = array();
    foreach ($results['files'] as $file) {
      $exists[] = $file['name'];
    }
    sort($exists);

    $this->assertEqual(
      array('empty', 'notempty'),
      $exists
    );

    unlink("$root/empty");

    // Wait for change to be observed
    $this->assertFileList($root, array(
      'notempty'
    ));

    $results = $this->watchmanCommand('query', $root, array(
      'expression' => 'exists'
    ));

    $this->assertEqual(
      'notempty',
      $results['files'][0]['name']
    );

    $results = $this->watchmanCommand('query', $root, array(
      'expression' => array('not', 'exists')
    ));

    $this->assertEqual(
      'empty',
      $results['files'][0]['name']
    );
  }

}

