<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class ignoreTestCase extends WatchmanTestCase {
  function testIgnoreGit() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/.git");
    mkdir("$root/.git/objects");
    mkdir("$root/.git/objects/pack");
    touch("$root/foo");

    $this->watch($root);
    // prove that we don't see pack in .git as we crawl
    $this->assertFileList($root, array(
      '.git',
      '.git/objects',
      'foo'
    ));

    // And prove that we aren't watching deeply under .git
    touch("$root/.git/objects/dontlookatme");
    $this->assertFileList($root, array(
      '.git',
      '.git/objects',
      'foo'
    ));
  }

  function testIgnoreGeneric() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    $cfg = array(
      'ignore_dirs' => array('build')
    );
    file_put_contents("$root/.watchmanconfig", json_encode($cfg));

    mkdir("$root/build");
    mkdir("$root/build/lower");
    mkdir("$root/builda");
    touch("$root/foo");
    touch("$root/build/bar");
    touch("$root/buildfile");
    touch("$root/build/lower/baz");
    touch("$root/builda/hello");

    $this->watch($root);
    $this->assertFileList($root, array(
      '.watchmanconfig',
      'builda',
      'builda/hello',
      'buildfile',
      'foo',
    ));

    touch("$root/build/lower/dontlookatme");
    touch("$root/build/orme");
    touch("$root/buil");
    $this->assertFileList($root, array(
      '.watchmanconfig',
      'buil',
      'builda',
      'builda/hello',
      'buildfile',
      'foo',
    ));

  }
}

