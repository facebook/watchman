<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class SuffixExprTestCase extends WatchmanTestCase {
  function testSuffixExpr() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    mkdir("$root/subdir");
    touch("$root/subdir/bar.txt");

    $this->watch($root);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => array('suffix', 'c'),
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo.c'), $res['files']);

    $res = $this->watchmanCommand('query', $root, array(
      'expression' => 'suffix',
      'fields' => array('name'),
    ));
    $this->assertEqual(
      'failed to parse query: must use ["suffix", "suffixstring"]',
      $res['error']
    );

  }
}

// vim:ts=2:sw=2:et:
