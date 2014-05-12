<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class illegalFSTypeTestCase extends WatchmanTestCase {
  function getGlobalConfig() {
    return array(
      'illegal_fstypes' => array(
        // This should include any/all fs types. If this test fails on
        // your platform, look in /tmp/watchman-test.log for a line like:
        // "path /var/tmp/a3osdzvzqnco0sok is on filesystem type zfs"
        // then add the type name to this list, in sorted order
        'cifs',
        'hfs',
        'nfs',
        'smb',
        'unknown',
        'zfs',
      ),
      'illegal_fstypes_advice' => 'just cos',
    );
  }

  function testIllegal() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $res = $this->watch($root, false);
    if (idx($res, 'error')) {
      $this->assertRegex(
        "/unable to resolve root .*: path uses the \".*\" filesystem ".
        "and is disallowed by global config illegal_fstypes: just cos/",
        $res['error']);
    } else {
      $this->assertEqual(
        "Look in /tmp/watchman-test.log for a line matching ".
        "'is on filesystem type XXX', then add the XXX string to the list ".
        "of types in fstype.php",
        'should not succeed in watching'
      );
    }
  }
}
