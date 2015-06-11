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
    $failed = 0;
    $colors = array(
      'pass' => '<fg:green>OK</fg>  ',
      'fail' => '<fg:red>FAIL</fg>',
      'skip' => '<fg:yellow>SKIP</fg>',
    );
    foreach ($results as $result) {
      $res = $result->getResult();
      $status = idx($colors, $res, $res);
      echo phutil_console_format("$status %s (%.2fs)\n",
        $result->getName(),
        $result->getDuration());
      if ($res == 'pass' || $res == 'skip') {
        continue;
      }
      echo $result->getUserData() . "\n";
      $failed++;
    }
    if (!$failed) {
      echo phutil_console_format("\nAll %d tests passed/skipped :successkid:\n",
        count($results));
    } else {
      echo phutil_console_format("\n%d of %d tests failed\n",
        $failed, count($results));
    }
    return $failed ? 1 : 0;
  }
}
// vim:ts=2:sw=2:et:
