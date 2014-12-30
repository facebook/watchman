<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerMaxFilesCase extends WatchmanTestCase {
  function testMaxFiles() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath('log');
    $env = $dir->getPath('env');
    $root = $dir->getPath('dir');

    mkdir($root);

    file_put_contents("$root/.watchmanconfig", json_encode(
      array('settle' => 200)
    ));

    $this->watch($root);

    $this->trigger($root,
      array(
        'name' => 'cap',
        'command' => array(
          PHP_BINARY,
          // Ubuntu disables 'E' by default, breaking this script
          '-d variables_order=EGPCS',
          '-d register_argc_argv=1',
          dirname(__FILE__) . DIRECTORY_SEPARATOR . '_capture.php',
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

    $this->waitForNoThrow(function () use ($env) {
      return file_exists($env) && filesize($env) > 0;
    });

    $this->assertEqual(true, file_exists($env));
    $data = file_get_contents($env);
    $this->assertEqual(true, strlen($data) > 0);
    $this->assertEqual(
      0,
      preg_match('/WATCHMAN_FILES_OVERFLOW/', $data),
      "WATCHMAN_FILES_OVERFLOW should not be in $env"
    );

    $observed = false;

    $deadline = time() + 5;
    while (time() < $deadline) {
      @unlink($log);
      @unlink($env);

      touch("$root/B.txt");
      touch("$root/A.txt");
      touch("$root/C.txt");
      touch("$root/D.txt");

      $obj = $this->waitForJsonInput($log);
      $this->assertEqual(2, count($obj));

      $envdata = @file_get_contents($env);
      if (preg_match('/WATCHMAN_FILES_OVERFLOW/', $envdata)) {
        $observed = true;
        break;
      }
    }

    $this->assertTrue($observed, "Observed an overflow");
  }
}
