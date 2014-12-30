<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class emptyExistsTestCase extends WatchmanTestCase {
  function testEmpty() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/empty");
    file_put_contents("$root/notempty", "foo");

    $this->watch($root);
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

    $clock = $results['clock'];
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

    // "files that don't exist" without a since term is absurd, so pass that in
    $results = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'expression' => array('not', 'exists')
    ));

    $this->assertEqual(
      'empty',
      $results['files'][0]['name']
    );
  }

}
