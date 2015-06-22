<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class sinceTestCase extends WatchmanTestCase {
  function testSinceIssue2() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $this->assertFileList($root, array());

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo');
    mkdir("$root/foo");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/111');
    touch("$root/foo/111");

    $this->assertFileListUsingSince($root, 'n:foo', array(
      'foo',
      'foo/111',
    ), null);

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo/bar');
    mkdir("$root/foo/bar");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/bar/222');
    touch("$root/foo/bar/222");

    $this->watchmanCommand('log', 'debug', 'XXX: wait to observe lists');


    $since = array(
      'foo/bar',
      'foo/bar/222'
    );
    if (PHP_OS == 'SunOS') {
      // This makes me sad, but Solaris reports the parent dir
      // as changed when we mkdir within it
      array_unshift($since, 'foo');
    }

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/111',
        'foo/bar',
        'foo/bar/222'
      ),
      $since
    );
    $this->watchmanCommand('log', 'debug', 'XXX: closing out');

  }

  function testSinceIssue1() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/111");
    touch("$root/222");

    $watch = $this->watch($root);
    $this->assertFileList($root, array(
      '111',
      '222'
    ));

    $initial = $this->watchmanCommand('since', $root, 'n:foo');

    mkdir("$root/bar");
    touch("$root/bar/333");

    // We should not observe 111 or 222 in this update
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        '111',
        '222',
        'bar',
        'bar/333'
      ),
      array(
        'bar',
        'bar/333'
      )
    );
  }

  function testSinceRelativeRoot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $watch = $this->watch($root);

    $clock = $this->watchmanCommand('clock', $root);
    $clock = $clock['clock'];

    touch("$root/a");
    mkdir("$root/subdir");
    touch("$root/subdir/foo");

    $this->assertFileList($root, array(
      'a',
      'subdir',
      'subdir/foo',
    ));

    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'relative_root' => 'subdir',
      'fields' => array('name'),
    ));
    $this->assertEqual(array('foo'), $res['files']);
    $clock = $res['clock'];

    // touch a file outside the relative root
    touch("$root/b");
    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'relative_root' => 'subdir',
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);
    $clock = $res['clock'];

    // touching just the subdir shouldn't cause anything to show up
    touch("$root/subdir");
    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'relative_root' => 'subdir',
      'fields' => array('name'),
    ));
    $this->assertEqual(array(), $res['files']);
    $clock = $res['clock'];

    // touching a new file inside the subdir should cause it to show up
    mkdir("$root/subdir/dir2");
    touch("$root/subdir/dir2/bar");
    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'relative_root' => 'subdir',
      'fields' => array('name'),
    ));
    sort($res['files']);
    $this->assertEqual(array('dir2', 'dir2/bar'), $res['files']);
  }

  function assertFreshInstanceForSince($root, $since, $empty) {
    $res = $this->watchmanCommand('query', $root, array(
      'since' => $since,
      'fields' => array('name'),
      'empty_on_fresh_instance' => $empty,
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    if ($empty) {
      $this->assertEqual(array(), $res['files']);
    } else {
      $this->assertEqual(array('111'), $res['files']);
    }
  }

  function testSinceFreshInstance() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $this->assertFileList($root, array());
    touch("$root/111");

    // no 'since' automatically means fresh instance
    $res = $this->watchmanCommand('query', $root, array(
      'fields' => array('name'),
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('111'), $res['files']);

    // relative clock value, fresh instance
    $this->assertFreshInstanceForSince($root, 'c:0:1:0:1', false);

    // old-style clock value (implies fresh instance, even if the pid is the
    // same)
    $pid = $this->watchman_instance->getProcessID();
    $this->assertFreshInstanceForSince($root, "c:$pid:1", false);

    // -- decompose clock and replace elements one by one
    $clock = $this->watchmanCommand('clock', $root);
    $clock = $clock['clock'];
    $p = explode(':', $clock);
    // 'c', $start_time, $pid, $root_number, $ticks
    $this->assertEqual(5, count($p));

    // replace start time
    $this->assertFreshInstanceForSince($root, "c:0:$p[2]:$p[3]:$p[4]", false);

    // replace pid
    $this->assertFreshInstanceForSince($root, "c:$p[1]:1:$p[3]:$p[4]", false);

    // replace root number (also try empty_on_fresh_instance)
    $this->assertFreshInstanceForSince($root, "c:$p[1]:$p[2]:0:$p[4]", true);

    // empty_on_fresh_instance, not a fresh instance
    touch("$root/222");

    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'fields' => array('name'),
      'empty_on_fresh_instance' => true,
    ));
    $this->assertEqual(false, $res['is_fresh_instance']);
    $this->assertEqual(array('222'), $res['files']);

    // fresh instance results should omit deleted files
    unlink("$root/111");
    $res = $this->watchmanCommand('query', $root, array(
      'since' => 'c:0:1:0:1',
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('222'), $res['files']);
  }

  function testReaddWatchFreshInstance() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $this->assertFileList($root, array());
    touch("$root/111");

    $res = $this->watchmanCommand('query', $root, array(
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('111'), $res['files']);

    $clock = $res['clock'];
    unlink("$root/111");
    $this->watchmanCommand('watch-del', $root);
    $res = $this->watchmanCommand('watch', $root);
    $this->assertEqual(NULL, idx($res, 'error'));
    touch("$root/222");

    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('222'), $res['files']);
  }

  function testRecrawlFreshInstance() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $this->assertFileList($root, array());
    touch("$root/111");

    $res = $this->watchmanCommand('query', $root, array(
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('111'), $res['files']);

    $clock = $res['clock'];
    unlink("$root/111");
    $this->watchmanCommand('debug-recrawl', $root);
    $this->assertEqual(NULL, idx($res, 'error'));
    touch("$root/222");

    $res = $this->watchmanCommand('query', $root, array(
      'since' => $clock,
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('222'), $res['files']);
    $this->assertRegex('/Recrawled this watch/', $res['warning']);
  }
}
