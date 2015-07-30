<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class queryDirNameTestCase extends WatchmanTestCase {
  function testDirName() {
    $dir = new WatchmanDirectoryFixture();
    $root = realpath($dir->getPath());
    for ($i = 0; $i < 5; $i++) {
      mkdir("$root/$i/$i/$i/$i/$i", 0777, true);
      touch("$root/a");
      touch("$root/$i/a");
      touch("$root/{$i}a");
      touch("$root/$i/$i/a");
      touch("$root/$i/$i/$i/a");
      touch("$root/$i/$i/$i/$i/a");
      touch("$root/$i/$i/$i/$i/$i/a");
    }

    $this->watch($root);

    $tests = array(
      array('', null, array(
        '0/0/0/0/0/a',
        '0/0/0/0/a',
        '0/0/0/a',
        '0/0/a',
        '0/a',
        '1/1/1/1/1/a',
        '1/1/1/1/a',
        '1/1/1/a',
        '1/1/a',
        '1/a',
        '2/2/2/2/2/a',
        '2/2/2/2/a',
        '2/2/2/a',
        '2/2/a',
        '2/a',
        '3/3/3/3/3/a',
        '3/3/3/3/a',
        '3/3/3/a',
        '3/3/a',
        '3/a',
        '4/4/4/4/4/a',
        '4/4/4/4/a',
        '4/4/4/a',
        '4/4/a',
        '4/a',
        'a',
      )),
      array('', 4, array(
        '0/0/0/0/0/a',
        '1/1/1/1/1/a',
        '2/2/2/2/2/a',
        '3/3/3/3/3/a',
        '4/4/4/4/4/a',
      )),
      array('', 3, array(
        '0/0/0/0/0/a',
        '0/0/0/0/a',
        '1/1/1/1/1/a',
        '1/1/1/1/a',
        '2/2/2/2/2/a',
        '2/2/2/2/a',
        '3/3/3/3/3/a',
        '3/3/3/3/a',
        '4/4/4/4/4/a',
        '4/4/4/4/a',
      )),
      array('0', null, array(
        '0/0/0/0/0/a',
        '0/0/0/0/a',
        '0/0/0/a',
        '0/0/a',
        '0/a',
      )),
      array('1', null, array(
        '1/1/1/1/1/a',
        '1/1/1/1/a',
        '1/1/1/a',
        '1/1/a',
        '1/a',
      )),
      array('1', 0, array(
        '1/1/1/1/1/a',
        '1/1/1/1/a',
        '1/1/1/a',
        '1/1/a',
      )),
      array('1', 1, array(
        '1/1/1/1/1/a',
        '1/1/1/1/a',
        '1/1/1/a',
      )),
      array('1', 2, array(
        '1/1/1/1/1/a',
        '1/1/1/1/a',
      )),
      array('1', 3, array(
        '1/1/1/1/1/a',
      )),
      array('1', 4, array(
      )),
    );

    foreach ($tests as $tdata) {
      list ($dirname, $depth, $expect) = $tdata;

      if ($depth === null) {
        $term = array('dirname', $dirname); // Equivalent to depth ge 0
      } else {
        $term = array('dirname', $dirname, array('depth', 'gt', $depth));
      }

      $label = json_encode(array('tdata' => $tdata, 'term' => $term));

      $results = $this->watchmanCommand('query', $root, array(
        'expression' => array('allof',
            $term,
            array('name', 'a')
          ),
        'fields' => array('name')
      ));
      if (isset($results['error'])) {
        $this->assertFailure($results['error'] . ' ' . $label);
      }
      $files = $results['files'];
      $this->assertEqualFileList($expect, $files, $label);
    }
  }
}
