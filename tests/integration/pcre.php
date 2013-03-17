<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class pcreTestCase extends WatchmanTestCase {

  function testPCRE() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array('bar.txt', 'foo.c'));

    $out = $this->watchmanCommand('find', $root, '-p', '.*c$');
    $this->assertEqual('foo.c', $out['files'][0]['name']);

    $out = $this->watchmanCommand('find', $root, '-p', '.*txt$');
    $this->assertEqual('bar.txt', $out['files'][0]['name']);

    // Cleanup for invalid pcre
    $out = $this->watchmanCommand('find', $root, '-p', '(');
    $this->assertEqual(
      "invalid rule spec: invalid pcre: `(' at offset 1: code 14 missing )",
      $out['error']
    );

    // Test case insensitive mode
    $out = $this->watchmanCommand('find', $root, '-P', '.*C$');
    $this->assertEqual('foo.c', $out['files'][0]['name']);
  }

}

