<?php

/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
