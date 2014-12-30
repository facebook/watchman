<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class invalidExprTestCase extends WatchmanTestCase {
  function testInvalidExprTerm() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    $this->watch($root);
    $results = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'allof',
        'dont-implement-this-term',
        array(
          'anyof',
          array(
            'suffix', 'apcarc'
          )
        )
      )
    ));

    $this->assertEqual(
      "failed to parse query: unknown expression term ".
      "'dont-implement-this-term'",
      $results['error']
    );
  }

  function testInvalidSyncTimeout() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    $this->watch($root);

    $results = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'exists',
      ),
      'sync_timeout' => -1,
    ));

    $this->assertEqual(
      "failed to parse query: sync_timeout must be an integer value >= 0",
      $results['error']
    );

    $results = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'exists',
      ),
      'sync_timeout' => 200,
    ));

    $this->assertEqual(array(), $results['files'], "parsed sync_timeout");
  }

}
