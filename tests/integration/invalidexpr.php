<?php

/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class invalidExprTestCase extends WatchmanTestCase {
  function testEmpty() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $this->watchmanCommand('watch', $root);
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

}


