<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerMaxFilesCase extends WatchmanTestCase {
  function testMaxFiles() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $log = $dir->getPath() . "log";
    $env = $dir->getPath() . "env";
    $root = realpath($dir->getPath()) . "/dir";

    mkdir($root);

    file_put_contents("$root/.watchmanconfig", json_encode(
      array('settle' => 200)
    ));

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
        'max_files_stdin' => 2,
      )
    );

    touch("$root/A.txt");

    $obj = $this->waitForJsonInput($log);
    $this->assertEqual(1, count($obj));

    $this->assertEqual(
      0,
      preg_match('/WATCHMAN_FILES_OVERFLOW/', file_get_contents($env)),
      "WATCHMAN_FILES_OVERFLOW should not be in $env"
    );

    $observed = false;

    $deadline = time() + 5;
    while (time() < $deadline) {
      unlink($log);
      unlink($env);

      touch("$root/B.txt");
      touch("$root/A.txt");
      touch("$root/C.txt");
      touch("$root/D.txt");

      $obj = $this->waitForJsonInput($log);
      $this->assertEqual(2, count($obj));

      $envdata = file_get_contents($env);
      if (preg_match('/WATCHMAN_FILES_OVERFLOW/', $envdata)) {
        $observed = true;
        break;
      }
    }

    $this->assertTrue($observed, "Observed an overflow");
  }
}
