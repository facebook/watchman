<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerChdirTestCase extends WatchmanTestCase {
  function testChdir() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $log = $dir->getPath() . "log";
    $env = $dir->getPath() . "env";
    $root = realpath($dir->getPath()) . "/dir";

    mkdir($root);
    mkdir("$root/sub");

    $this->watch($root);

    $out = $this->trigger($root,
      array(
        'name' => 'cap',
        'command' => array(
          dirname(__FILE__) . '/capture.sh',
          $log,
          $env
        ),
        'expression' => array('suffix', 'txt'),
        'stdin' => array('name'),
        'chdir' => 'sub',
      )
    );

    touch("$root/A.txt");

    $obj = $this->waitForJsonInput($log);
    $this->assertEqual(1, count($obj));

    $this->assertRegex(",PWD=$root/sub,", file_get_contents($env));
  }
}
