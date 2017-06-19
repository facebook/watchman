<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class twodeepTestCase extends WatchmanTestCase {
  function testTwoDeep() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);

    $this->assertFileList($root, array());

    $this->assertEqual(true, mkdir("$root/foo"));
    $this->assertEqual(true, mkdir("$root/foo/bar"));

    // Guarantee that 111's mtime is greater than its directory's
    sleep(1);
    $this->assertEqual(3, file_put_contents("$root/foo/bar/111", "111"));
    $this->watchmanCommand('log', 'debug', 'XXX: created 111');

    $this->assertFileList($root, array(
      "foo",
      "foo/bar",
      "foo/bar/111"
    ));

    $query = $this->watchmanCommand('find', $root, 'foo/bar/111');
    $wfile = $query['files'][0];
    clearstatcache();
    $sfile = stat("$root/foo/bar/111");

    $query = $this->watchmanCommand('find', $root, 'foo/bar');
    $wdir = $query['files'][0];
    clearstatcache();
    $sdir = stat("$root/foo/bar");

    $this->watchmanCommand('log', 'debug', 'XXX: perform assertions');

    $compare_fields = array('size', 'mode', 'uid', 'gid',
                            'mtime', 'ctime');
    if (!phutil_is_windows()) {
      // These are meaningless in msvcrt, so no sense in comparing them
      $compare_fields[] = 'dev';
      $compare_fields[] = 'ino';
      // We can only get this by directly examining a file, but not implicitly
      // when crawling the dir
      $compare_fields[] = 'nlink';
    }
    foreach ($compare_fields as $field) {
      $this->assertEqual($sfile[$field], $wfile[$field],
        "file: $field {$sfile[$field]} vs watchman {$wfile[$field]} " . json_encode($wfile));
      $this->assertEqual($sdir[$field], $wdir[$field],
        "dir: $field {$sdir[$field]} vs watchman {$wdir[$field]}");
    }

    $this->watchmanCommand('log', 'debug', 'XXX: remove it all');
    w_rmdir_recursive("$root/foo/bar");

    $this->assertFileList($root, array(
      "foo",
    ));
  }
}
