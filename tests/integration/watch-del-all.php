<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class watchDelAllTestCase extends WatchmanTestCase {
  function testWatchDelAll() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $files = array('a', 'b', 'c', 'd', 'e');
    $dirs = array_map(
      function ($file) use($root) {
        return "$root/$file";
      }, 
      $files
    );

    foreach ($dirs as $d) {
      mkdir($d);

      foreach ($files as $f) {
        touch("$d/$f");
      }

      $this->watch($d);
      $this->assertFileList($d, $files); 
    }

    $resp = $this->watchmanCommand('watch-list');
    $watched = $resp['roots'];
    sort($watched);

    $this->assertEqual($dirs, $watched);

    $this->watchmanCommand('watch-del-all');

    $this->waitForWatchman(
      array('watch-list'),
      function ($resp) {
        return 0 == count($resp['roots']);
      }
    );
  }
}
