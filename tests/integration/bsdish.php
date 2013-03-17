<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
class bsdishTestCase extends WatchmanTestCase {

  // Verify that we don't generate spurious change observations
  // when we delete files at the top level
  function testBSDishTopLevel() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    mkdir("$root/lower");
    touch("$root/lower/file");
    touch("$root/top");

    $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array(
      'lower',
      'lower/file',
      'top'
    ));

    $find = $this->watchmanCommand('find', $root);
    $clock = $find['clock'];

    $since = $this->watchmanCommand('since', $root, $clock);
    $clock = $since['clock'];

    $since = $this->watchmanCommand('since', $root, $clock);
    $this->assertEqual(array(), $since['files']);
    $clock = $since['clock'];

    unlink("$root/top");

    $this->assertFileList($root, array(
      'lower',
      'lower/file'
    ));

    $now = $this->watchmanCommand('since', $root, $clock);

    //print_r($now);
    $this->assertEqual(1, count($now['files']));
    $this->assertEqual('top', $now['files'][0]['name']);
    $this->assertEqual(false, $now['files'][0]['exists']);
  }
}

