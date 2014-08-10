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
      $expect = array('a/lemon');
      if (PHP_OS == 'SunOS') {
        // This makes me sad, but Solaris reports the parent dir
        // as changed, too
        array_unshift($expect, 'a');
      }
      $this->assertEqual($expect, $sub['files']);

      // trigger a recrawl, make sure the subscription isn't lost
      $this->watchmanCommand('debug-recrawl', $root);
      $subs = $this->waitForSub('myname', function ($data) {
        foreach ($data as $ent) {
          if (!idx($ent, 'is_fresh_instance')) {
            continue;
          }
          $files = $ent['files'];
          sort($files);
          if ($files === array('a', 'b')) {
            return true;
          }
        }
        return false;;
      });

      $this->assertEqual(true, count($subs) > 0, 'got fresh instance response');

      // Ensure that we can pick up the recrawl warning in the subscription
      // stream.  We can't guarantee which element it will be, so loop.
      $warn = null;
      foreach ($subs as $sub) {
        if (isset($sub['warning'])) {
          $warn = $sub['warning'];
          break;
        }
      }
      $this->assertRegex('/Recrawled this watch/', $warn);

      $this->watchmanCommand('unsubscribe', $root, 'myname');
    } catch (Exception $e) {
      $this->watchmanCommand('unsubscribe', $root, 'myname');
      throw $e;
    }
  }
}
