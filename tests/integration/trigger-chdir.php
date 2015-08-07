<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerChdirTestCase extends WatchmanTestCase {
  function testChdir() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $env = $dir->getPath("env");
    $root = $dir->getPath("dir");

    mkdir($root);
    mkdir("$root/sub");

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
        'chdir' => 'sub',
      )
    );
    touch("$root/A.txt");

    $obj = $this->waitForJsonInput($log);
    $this->assertEqual(1, count($obj));
    $root_pat = preg_quote($root . DIRECTORY_SEPARATOR . 'sub');

    $this->waitFor(
      function () use ($env, $root, $root_pat) {
        $envdata = file_get_contents($env);
        return preg_match(",PWD=$root_pat,i", $envdata) > 0;
      },
      10,
      "waiting for PWD to show in $env log file ($root_pat)"
    );

    $envdata = file_get_contents($env);
    $this->assertRegex(",PWD=$root_pat,i", $envdata);
    $this->assertRegex("/WATCHMAN_EMPTY_ENV_VAR=$/m", $envdata);
  }

  function testChdirRelativeRoot() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath() . "log";
    $env = $dir->getPath() . "env";
    $root = realpath($dir->getPath()) . "/dir";

    mkdir($root);
    mkdir("$root/sub1");
    mkdir("$root/sub1/sub2");

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
        'relative_root' => 'sub1',
        'chdir' => 'sub2',
      )
    );
    touch("$root/sub1/A.txt");

    $obj = $this->waitForJsonInput($log);
    $this->assertEqual(1, count($obj));

    $root_pat = preg_quote(w_normalize_filename($root) .
                           DIRECTORY_SEPARATOR . 'sub1' .
                           DIRECTORY_SEPARATOR . 'sub2');
    $this->waitFor(
      function () use ($env, $root, $root_pat) {
        $envdata = @file_get_contents($env);
        $root = preg_quote($root);
        return preg_match(",PWD=$root_pat,i", $envdata) == 1;
      },
      10,
      function () use ($env, $root, $root_pat) {
        $envdata = @file_get_contents($env);
        return "$envdata\nwaiting for PWD to show in $env log file ".
          "(pat: $root_pat)";
      }
    );

    $envdata = file_get_contents($env);
    $sub1_pat = preg_quote(w_normalize_filename("$root/sub1"));
    $root_pat = preg_quote(w_normalize_filename($root));
    $this->assertRegex(",PWD=$root_pat,i", $envdata);
    $this->assertRegex("/WATCHMAN_EMPTY_ENV_VAR=$/m", $envdata);
    $this->assertRegex(",^WATCHMAN_ROOT=$root_pat$,mi", $envdata);
    $this->assertRegex(",^WATCHMAN_RELATIVE_ROOT=$sub1_pat$,mi", $envdata);
  }
}
