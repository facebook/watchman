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

    // relative clock value with non-matching pid
    $res = $this->watchmanCommand('query', $root, array(
      'since' => 'c:1:1',
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('111'), $res['files']);

    // empty_on_fresh_instance, fresh instance
    $res = $this->watchmanCommand('query', $root, array(
      'since' => 'c:1:1',
      'fields' => array('name'),
      'empty_on_fresh_instance' => true,
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array(), $res['files']);

    // empty_on_fresh_instance, not a fresh instance
    touch("$root/222");
    $clock = $res['clock'];

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
      'since' => 'c:1:1',
      'fields' => array('name')
    ));
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(array('222'), $res['files']);
  }
}
