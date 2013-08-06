<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class subscribeTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function testSubscribe() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/a");
    touch("$root/a/lemon");
    touch("$root/b");

    $this->watch($root);

    $this->assertFileList($root, array(
      'a',
      'a/lemon',
      'b'
    ));

    try {
      $sub = $this->watchmanCommand('subscribe', $root, 'myname', array(
        'fields' => array('name'),
      ));

      $this->waitForSub('myname', function ($data) {
        return true;
      });
      list($sub) = $this->getSubData('myname');

      $this->assertEqual(true, $sub['is_fresh_instance']);
      $files = $sub['files'];
      sort($files);
      $this->assertEqual(array('a', 'a/lemon', 'b'), $files);

      // delete a file and see that subscribe tells us about it
      unlink("$root/a/lemon");
      $this->waitForSub('myname', function ($data) {
        return true;
      });
      list($sub) = $this->getSubData('myname');

      $this->assertEqual(false, $sub['is_fresh_instance']);
      $this->assertEqual(array('a/lemon'), $sub['files']);

      $this->watchmanCommand('unsubscribe', $root, 'myname');
    } catch (Exception $e) {
      $this->watchmanCommand('unsubscribe', $root, 'myname');
      throw $e;
    }
  }
}
