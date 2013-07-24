<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class otherCookiesTestCase extends WatchmanTestCase {
  function testOtherCookies() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $host = gethostname();
    $pid = WatchmanInstance::get()->getProcessID();

    $this->assertFileList($root, array());

    $this->assertEqual(true, mkdir("$root/foo"));

    // Same process, same watch
    $this->assertEqual(true, touch("$root/.watchman-cookie-$host-$pid-100000"));
    $diff_cookies = array(
      // Same process, different watch root
      "foo/.watchman-cookie-$host-$pid-100000",
      // Different process, same watch root
      ".watchman-cookie-$host-1-100000",
      // Different process, different watch root
      "foo/.watchman-cookie-$host-1-100000"
    );

    foreach ($diff_cookies as $cookie) {
      $this->assertEqual(true, touch("$root/$cookie"));
    }

    $this->assertFileList($root, array_merge(array('foo'),
                                             $diff_cookies));
  }
}
