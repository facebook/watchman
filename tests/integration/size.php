<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class sizeCompareTestCase extends WatchmanTestCase {
  function testSize() {
    $dir = new WatchmanDirectoryFixture();
    $root = realpath($dir->getPath());

    touch("$root/empty");
    file_put_contents("$root/notempty", "foo");

    $fp = fopen("$root/1k", "w");
    ftruncate($fp, 1024);
    fclose($fp);

    $this->watch($root);

    $tests = array(
      array('eq', 0, array('empty')),
      array('ne', 0, array('1k', 'notempty')),
      array('gt', 0, array('1k', 'notempty')),
      array('gt', 2, array('1k', 'notempty')),
      array('ge', 3, array('1k', 'notempty')),
      array('gt', 3, array('1k')),
      array('le', 3, array('empty', 'notempty')),
      array('lt', 3, array('empty')),
    );

    foreach ($tests as $tdata) {
      list ($op, $operand, $expect) = $tdata;

      $results = $this->watchmanCommand('query', $root, array(
        'expression' => array(
          'size', $op, $operand
        ),
        'fields' => array('name'),
      ));
      $files = $results['files'];
      sort($files);
      $this->assertEqual($expect, $files, json_encode($tdata));
    }

    unlink("$root/1k");
    $results = $this->watchmanCommand('query', $root, array(
      'expression' => array(
        'size', 'gt', 100
      ),
      'fields' => array('name'),
    ));
    $files = $results['files'];
    $this->assertEqual(array(), $results['files'], 'removed file');
  }
}
