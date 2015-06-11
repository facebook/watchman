<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanTestWorkflow extends ArcanistBaseWorkflow {
  public function getWorkflowName() {
    return 'test';
  }

  public function getCommandSynopses() {
    return '';
  }

  public function getCommandHelp() {
    return 'Runs integration tests';
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
    $srcdir = dirname(__FILE__) . '/../../';
    $engine = new WatchmanIntegrationEngine();
    $engine->setProjectRoot($srcdir);
    $paths = $this->getArgument('args');
    $results = $engine->run($paths);
    $ok = 0;
    foreach ($results as $result) {
      $pass = $result->getResult() == 'pass';
      $status = $pass ? '<fg:green>OK</fg>  ' : '<fg:red>FAIL</fg>';
      echo phutil_console_format("$status %s (%.2fs)\n",
        $result->getName(),
        $result->getDuration());
      if ($pass) {
        $ok++;
      } else {
        echo $result->getUserData() . "\n";
      }
    }
    $success = $ok == count($results);
    if ($success) {
      echo phutil_console_format("\nAll %d tests passed :successkid:\n",
        count($results));
    } else {
      echo phutil_console_format("\n%d of %d tests failed\n",
        count($results) - $ok, count($results));
    }
    return $success ? 0 : 1;
  }
}
// vim:ts=2:sw=2:et:
