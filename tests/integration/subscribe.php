<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class subscribeTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function testImmediateSubscribe() {
    $dir = new WatchmanDirectoryFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/.hg");

    $this->watch($root);
    $this->assertFileList($root, array('.hg'));
    try {
      $sub = $this->watchmanCommand('subscribe', $root, 'nodefer', array(
        'fields' => array('name', 'exists'),
        'defer_vcs' => false,
      ));

      $this->waitForSub('nodefer', function ($data) {
        return true;
      });
      list($sub) = $this->getSubData('nodefer');

      $this->assertEqual(true, $sub['is_fresh_instance']);
      $files = $sub['files'];
      $this->assertEqual(
        array(
          array('name' => '.hg', 'exists' => true)
        ), $files);

      touch("$root/.hg/wlock");
      $this->waitForSub('nodefer', function ($data) {
        return true;
      });
      $sub = $this->tail($this->getSubData('nodefer'));
      $wlock = null;
      foreach ($sub['files'] as $ent) {
        if ($ent['name'] == '.hg/wlock') {
          $wlock = $ent;
        }
      }
      $this->assertEqual(array('name' => w_normalize_filename('.hg/wlock'),
          'exists' => true), $ent);

      unlink("$root/.hg/wlock");

      $this->waitForSub('nodefer', function ($data) {
        return true;
      });
      $sub = $this->tail($this->getSubData('nodefer'));

      $wlock = null;
      foreach ($sub['files'] as $ent) {
        if ($ent['name'] == '.hg/wlock') {
          $wlock = $ent;
        }
      }
      $this->assertEqual(array('name' => w_normalize_filename('.hg/wlock'),
            'exists' => false), $ent);

      $this->watchmanCommand('unsubscribe', $root, 'nodefer');
    } catch (Exception $e) {
      $this->watchmanCommand('unsubscribe', $root, 'nodefer');
      throw $e;
    }
  }

  function tail(array $array) {
    return end($array);
  }

  function testSubscribe() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
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

      $relative_sub = $this->watchmanCommand('subscribe', $root, 'relative',
        array(
          'fields' => array('name'),
          'relative_root' => 'a',
        ));

      $this->waitForSub('myname', function ($data) {
        return true;
      });
      list($sub) = $this->getSubData('myname');

      $this->assertEqual(true, $sub['is_fresh_instance']);
      $files = $sub['files'];
      sort($files);
      $this->assertEqualFileList(array('a', 'a/lemon', 'b'), $files);

      $this->waitForSub('relative', function ($data) {
        return true;
      });
      list($sub) = $this->getSubData('relative');

      $this->assertEqual(true, $sub['is_fresh_instance']);
      $files = $sub['files'];
      $this->assertEqual(array('lemon'), $files);

      // delete a file and see that subscribe tells us about it.
      unlink("$root/a/lemon");
      $this->waitForSub('myname', function ($data) {
        return w_find_subdata_containing_file($data, 'a/lemon') !== null;
      });
      $sub = w_find_subdata_containing_file(
          $this->getSubData('myname'), 'a/lemon');
      $this->assertTrue(is_array($sub), 'missing `myname` subscription data');
      $this->assertEqual(false, $sub['is_fresh_instance']);

      $this->waitForSub('relative', function ($data) {
        return true;
      });
      $sub = $this->tail($this->getSubData('relative'));

      $this->assertEqual(false, $sub['is_fresh_instance']);
      $this->assertEqual(array('lemon'), $sub['files']);

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
      $this->watchmanCommand('unsubscribe', $root, 'relative');
    } catch (Exception $e) {
      $this->watchmanCommand('unsubscribe', $root, 'myname');
      $this->watchmanCommand('unsubscribe', $root, 'relative');
      throw $e;
    }
  }
}
