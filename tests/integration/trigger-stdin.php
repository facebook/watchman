<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerStdinTestCase extends WatchmanTestCase {
  function catCommand() {
    return array(
      PHP_BINARY,
      '-d register_argc_argv=1',
      dirname(__FILE__) . DIRECTORY_SEPARATOR . '_cat.php'
    );
  }

  function xtestNameListTrigger() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $root = $dir->getPath("dir");

    mkdir($root);

    $this->watch($root);

    $this->trigger($root,
      array(
        'name' => 'cat',
        'command' => $this->catCommand(),
        'expression' => array('suffix', 'txt'),
        'stdin' => 'NAME_PER_LINE',
        'stdout' => ">$log"
      )
    );

    touch("$root/A.txt");
    $this->assertFileContents($log, "A.txt\n");

    touch("$root/B.txt");
    touch("$root/A.txt");

    $lines = $this->waitForFileToHaveNLines($log, 2);
    sort($lines);
    $this->assertEqual(array("A.txt", "B.txt"), $lines);
  }

  function xtestAppendTrigger() {
    if (phutil_is_windows()) {
      $this->assertSkipped('no O_APPEND on windows');
    }
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $root = $dir->getPath("dir");

    mkdir($root);

    $this->watch($root);

    $this->trigger($root,
      array(
        'name' => 'cat',
        'command' => $this->catCommand(),
        'expression' => array('suffix', 'txt'),
        'stdin' => 'NAME_PER_LINE',
        'stdout' => ">>$log"
      )
    );

    touch("$root/A.txt");
    $this->assertFileContents($log, "A.txt\n");

    touch("$root/B.txt");
    $lines = $this->waitForFileToHaveNLines($log, 2);
    sort($lines);
    $this->assertEqual(array("A.txt", "B.txt"), $lines);
  }

  function testTriggerRelativeRoot() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $env = $dir->getPath("env");
    $root = $dir->getPath("dir");

    mkdir($root);
    mkdir("$root/subdir");

    $this->watch($root);

    // The command also xtests and prints out the cwd that the bash process was
    // invoked with, to make sure that the cwd is the subdirectory.
    $this->trigger($root, array(
      'name' => 'pwd cat',
      'command' => array(
          PHP_BINARY,
          '-d register_argc_argv=1',
          dirname(__FILE__) . DIRECTORY_SEPARATOR . '_capture.php',
          $log,
          $env
      ),
      'relative_root' => 'subdir',
      'expression' => array('suffix', 'txt'),
      'stdin' => 'NAME_PER_LINE',
    ));

    touch("$root/A.txt");
    touch("$root/subdir/B.txt");

    $root_pat = preg_quote(w_normalize_filename($root) .
                           DIRECTORY_SEPARATOR . 'subdir');
    $this->waitFor(
      function () use ($env, $root, $root_pat) {
        $envdata = @file_get_contents($env);
        return preg_match(",PWD=$root_pat,i", $envdata) > 0;
      },
      10,
      function () use ($env, $root_pat) {
        $envdata = @file_get_contents($env);
        return $envdata . "\n".
          "waiting for PWD to show in $env log file ($root_pat)";
      }
    );
    $envdata = file_get_contents($env);
    $this->assertRegex(",PWD=$root_pat,i", $envdata);

    $this->assertFileContents($log, "B.txt\n\n");
  }

  function xtestJsonNameOnly() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $root = $dir->getPath("dir");

    mkdir($root);

    $this->watch($root);

    $this->trigger($root,
      array(
        'name' => 'cat',
        'command' => $this->catCommand(),
        'expression' => array('suffix', 'txt'),
        'stdin' => array('name'),
        'stdout' => ">$log"
      )
    );

    touch("$root/A.txt");
    $this->assertFileContents($log, "[\"A.txt\"]\n");
  }

  function xtestJsonNameAndSize() {
    $dir = new WatchmanDirectoryFixture();
    $log = $dir->getPath("log");
    $root = $dir->getPath("dir");

    mkdir($root);

    $this->watch($root);

    $this->trigger($root,
      array(
        'name' => 'cat',
        'command' => $this->catCommand(),
        'expression' => array('suffix', 'txt'),
        'stdin' => array('name', 'size'),
        'stdout' => ">$log"
      )
    );

    touch("$root/A.txt");
    $this->assertFileContents($log, '[{"name": "A.txt", "size": 0}]'."\n");
  }
}
