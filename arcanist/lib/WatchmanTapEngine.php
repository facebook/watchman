<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanTapEngine {
  private $projectRoot;

  public function setProjectRoot($root) {
    $this->projectRoot = $root;
  }

  protected function getProjectRoot() {
    return $this->projectRoot;
  }

  public function run($tests) {
    return $this->runUnitTests($tests);
  }

  public function getEnableCoverage() {
    return false;
  }

  public function runUnitTests($tests) {
    // Now find all the test programs
    $root = $this->getProjectRoot();
    $test_dir = $root . "/tests/";
    $futures = array();

    if (!$tests) {
      $paths = glob($test_dir . "*.t");
    } else {
      $paths = array();
      foreach ($tests as $path) {
        $tpath = preg_replace('/\.c$/', '.t', $path);
        if (preg_match("/\.c$/", $path) && file_exists($tpath)) {
          $paths[] = realpath($tpath);
        }
      }
    }

    foreach ($paths as $test) {
      $relname = substr($test, strlen($test_dir));
      $futures[$relname] = new ExecFuture($test);
    }

    $results = array();
    $futures = new FutureIterator($futures);
    foreach ($futures->limit(4) as $test => $future) {
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

  protected function supportsRunAllTests() {
    return true;
  }
}

// vim:ts=2:sw=2:et:
