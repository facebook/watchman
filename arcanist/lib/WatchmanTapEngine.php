<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanTapEngine extends ArcanistBaseUnitTestEngine {
  private $projectRoot;

  protected function getProjectRoot() {
    if (!$this->projectRoot) {
      $this->projectRoot = $this->getWorkingCopy()->getProjectRoot();
    }
    return $this->projectRoot;
  }

  protected function make($target) {
    return execx("cd %s && make %s",
      $this->getProjectRoot(), $target);
  }

  public function run() {
    return $this->runUnitTests();
  }

  public function runUnitTests() {
    // Build any unit tests
    $this->make('build-tests');

    // Now find all the test programs
    $root = $this->getProjectRoot();
    $test_dir = $root . "/tests/";
    $futures = array();

    foreach (glob($test_dir . "*.t") as $test) {
      $relname = substr($test, strlen($test_dir));
      $futures[$relname] = new ExecFuture($test);
    }

    $results = array();
    foreach (Futures($futures)->limit(4) as $test => $future) {
      list($err, $stdout, $stderr) = $future->resolve();

      $results[] = $this->parseTestResults(
        $test, $err, $stdout, $stderr);
    }

    return $results;
  }

  private function parseTestResults($test, $err, $stdout, $stderr) {
    $result = new ArcanistUnitTestResult();
    $result->setName($test);
    $result->setUserData($stdout . $stderr);
    $result->setResult($err == 0 ?
      ArcanistUnitTestResult::RESULT_PASS :
      ArcanistUnitTestResult::RESULT_FAIL
    );
    if (preg_match("/# ELAPSED: (\d+)ms/", $stderr, $M)) {
      $result->setDuration($M[1] / 1000);
    }

    return $result;
  }
}

// vim:ts=2:sw=2:et:
