<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class rootRestrictTestCase extends WatchmanTestCase {
  function getGlobalConfig() {
    return array(
      'root_restrict_files' => array('.git', '.hg', '.foo', '.bar')
    );
  }

  function testRootRestrict() {
    $passes = array(
      array("directory", ".git"),
      array("directory", ".hg"),
      array("file", ".foo"),
      array("file", ".bar"),
      array("directory", ".bar")
    );
    $fails = array(
      NULL,
      array("directory", ".svn"),
      array("file", "baz")
    );

    foreach ($passes as $p) {
      $dir = PhutilDirectoryFixture::newEmptyFixture();
      $root = realpath($dir->getPath());

      if ($p[0] === "directory") {
        mkdir("$root/$p[1]");
      } else {
        touch("$root/$p[1]");
      }
      $res = $this->watch($root);

      // Make sure the watch actually happened
      touch("$root/f");
      $this->assertFileList($root, array(
        'f',
        $p[1]
      ));
    }

    foreach ($fails as $f) {
      $dir = PhutilDirectoryFixture::newEmptyFixture();
      $root = realpath($dir->getPath());

      if ($f) {
        if ($f[0] === "directory") {
          mkdir("$root/$f[1]");
        } else {
          touch("$root/$f[1]");
        }
      }

      $res = $this->watch($root, false);
      $this->assertEqual("unable to resolve root $root: none of the files " .
                         "listed in global config root_files are " .
                         "present and enforce_root_files is set to true",
                         $res['error']);
    }
  }
}
