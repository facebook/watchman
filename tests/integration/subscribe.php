<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class subscribeTestCase extends WatchmanTestCase {
  function testSubscribe() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/a");
    touch("$root/a/lemon");
    touch("$root/b");

    $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array(
      'a',
      'a/lemon',
      'b'
    ));

    try {
      $sub = $this->watchmanCommand('subscribe', $root, 'myname', array(
        'fields' => array('name'),
      ));

      list($sub) = $this->waitForSub('myname', function ($data) {
        return true;
      });

      $files = $sub['files'];
      sort($files);
      $this->assertEqual(array('a', 'a/lemon', 'b'), $files);

      $this->watchmanCommand('unsubscribe', $root, 'myname');
    } catch (Exception $e) {
      $this->watchmanCommand('unsubscribe', $root, 'myname');
      throw $e;
    }
  }
}
