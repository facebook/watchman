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

  public function run() {
    $this->projectRoot = $this->getWorkingCopy()->getProjectRoot();
    chdir($this->projectRoot);

    // Ensure that the test programs are up to date
    $res = 0;
    $output = array();
    exec("make build-tests 2>&1", $output, $res);
    if ($res) {
      $res = new ArcanistUnitTestResult();
      $res->setName('make build-tests');
      $res->setResult(ArcanistUnitTestResult::RESULT_BROKEN);
      $res->setUserData(implode("\n", $output));

      return array($res);
    }

    // Now find all the test programs
    $test_dir = $this->projectRoot . "/tests/";
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

    return $result;
  }
}

// vim:ts=2:sw=2:et:
