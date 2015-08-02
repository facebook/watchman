<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanTapWorkflow extends ArcanistBaseWorkflow {
  public function getWorkflowName() {
    return 'tap';
  }

  public function getCommandSynopses() {
    return '';
  }

  public function getCommandHelp() {
    return 'Runs an integration test with TAP output';
  }

  public function requiresConduit() {
    return false;
  }

  public function requiresRepositoryAPI() {
    return false;
  }

  public function requiresAuthentication() {
    return false;
  }

  public function getArguments() {
    return array('*' => 'args');
  }

  public function run() {
    $srcdir = realpath(dirname(__FILE__) . '/../../');
    chdir($srcdir);
    $engine = new WatchmanIntegrationEngine();

    // Hook up to running watchman instance from the python test harness
    if (getenv('WATCHMAN_SOCK')) {
      $engine->setWatchmanInstance(new PythonProvidedWatchmanInstance());
    }

    $engine->setProjectRoot($srcdir);
    $paths = $this->getArgument('args');
    $results = $engine->run($paths);
    $failed = 0;
    $formats = array(
      'pass' => "ok %d - %s %s\n",
      'fail' => "not ok %d - %s %s\n",
      'skip' => "ok %d # skip %s %s\n",
    );
    printf("1..%d\n", count($results));
    foreach ($results as $testno => $result) {
      $res = $result->getResult();
      $format = idx($formats, $res, $res);
      $output = explode("\n", $result->getUserData());
      if ($res == 'fail') {
        # makes the failed assertions render more nicely
        $output[] = 'failed ';
      }
      $last_line = array_pop($output);
      if ($output) {
        echo '# ' . implode("\n# ", $output) . "\n";
      }
      printf($format, $testno + 1, $result->getName(), $last_line);
    }
    return $failed ? 1 : 0;
  }
}
// vim:ts=2:sw=2:et:
