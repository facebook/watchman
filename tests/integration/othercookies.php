<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class otherCookiesTestCase extends WatchmanTestCase {
  function testOtherCookies() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    mkdir("$root/.git");
    $watch = $this->watch($root);
    $host = gethostname();
    $pid = $this->watchman_instance->getProcessID();

    $this->assertFileList($root, array('.git'));

    $this->assertEqual(true, mkdir("$root/foo"));

    // Same process, same watch
    $this->assertEqual(true,
                       touch("$root/.git/.watchman-cookie-$host-$pid-100000"));
    $diff_cookies = array(
      // Same process, different watch root
      "foo/.watchman-cookie-$host-$pid-100000",
      // Same process, root dir instead of VCS dir
      ".watchman-cookie-$host-$pid-100000",
      // Different process, same watch root
      ".git/.watchman-cookie-$host-1-100000",
      // Different process, root dir instead of VCS dir
      ".watchman-cookie-$host-1-100000",
      // Different process, different watch root
      "foo/.watchman-cookie-$host-1-100000",
    );

    foreach ($diff_cookies as $cookie) {
      $this->assertEqual(true, touch("$root/$cookie"));
    }

    $this->assertFileList($root, array_merge(array('foo', '.git'),
                                             $diff_cookies));
  }
}
