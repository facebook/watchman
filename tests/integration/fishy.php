<?php

/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class fishyTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function testFishy() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/foo");
    touch("$root/foo/a");
    $watch = $this->watch($root);

    $base = $this->watchmanCommand('find', $root, '.');
    // This is "c:PID:1" because nothing has changed in $root yet
    $clock = $base['clock'];

    $this->setLogLevel('debug');
    $this->watchmanCommand('log', 'debug', 'testFishy:START');

    system(
      "cd $root; ".
      "mv foo bar; ".
      "ln -s bar foo"
    );

    $this->assertFileListUsingSince($root, $clock,
      array(
        'bar',
        'bar/a',
        'foo'
      )
    );

    $this->watchmanCommand('log', 'debug', 'testFishy:END');
    $this->assertWaitForLog('/testFishy:END/');
    $this->setLogLevel('off');
    $on = false;
    $log = array();
    foreach (WatchmanInstance::get()->getLogData() as $item) {
      if (preg_match('/testFishy:START/', $item)) {
        $on = true;
      } else if (preg_match('/testFishy:END/', $item)) {
        break;
      }
      if ($on && preg_match('/fishy/', $item)) {
        $log[] = $item;
      }
    }

    $this->assertEqual(array(), $log, 'nothing fishy');
  }
}

