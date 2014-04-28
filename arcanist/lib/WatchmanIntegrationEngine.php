<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Runs both the integration and the unit tests
class WatchmanIntegrationEngine extends WatchmanTapEngine {

  public function run() {
    $unit = $this->runUnitTests();
    $integ = $this->runIntegrationTests();

    return array_merge($unit, $integ);
  }

  public function runIntegrationTests() {
    $this->make('all');

    // Now find all the test programs
    $root = $this->getProjectRoot();
    $test_dir = $root . "/tests/integration/";

    if ($this->getRunAllTests()) {
      $paths = glob($test_dir . "*.php");
      if (is_dir("python/bser/build")) {
        foreach (glob('python/bser/*.py') as $file) {
          $paths[] = $file;
        }
      }
    } else {
      $paths = $this->getPaths();
    }

    foreach (array(
      '/tmp/watchman-test.log',
      '/tmp/watchman-valgrind.log',
      '/tmp/watchman-valgrind.xml',
      '/tmp/watchman-callgrind.txt',
    ) as $log) {
      @unlink($log);
    }

    foreach ($paths as $path) {
      if (preg_match("/\.php$/", $path) && file_exists($path)) {
        require_once $path;
      }
    }

    $coverage = $this->getEnableCoverage();
    $instances = array(new WatchmanInstance($root, $coverage));

    // Exercise the different serialization combinations
    $cli_matrix = array(
      'bser/json' => '--server-encoding=bser --output-encoding=json',
      'json/json' => '--server-encoding=json --output-encoding=json',
    );

    // Find all the test cases that were declared
    $results = array();
    foreach (get_declared_classes() as $name) {
      $ref = new ReflectionClass($name);
      if (!$ref->isSubclassOf('WatchmanTestCase')) {
        continue;
      }

      // Good enough; let's use it
      $test_case = newv($name, array());
      $config = $test_case->getGlobalConfig();
      if ($config) {
        $instance = new WatchmanInstance($root, $coverage, $config);
        $instances[] = $instance;
      } else {
        $instance = $instances[0];
      }
      $test_case->setWatchmanInstance($instance);

      if (!$instance->getProcessID()) {
        $res = new ArcanistUnitTestResult();
        $res->setName('dead');
        $res->setUserData('died before test start');
        $res->setResult(ArcanistUnitTestResult::RESULT_FAIL);
        $results[] = array($res);
        break;
      }

      $test_case->setRoot($root);
      $test_case->setPaths($paths);
      $results[] = $test_case->run();

      if (!$test_case->needsLiveConnection()) {
        foreach ($cli_matrix as $mname => $args) {
          $test_case->useCLI($args);
          $cli_results = $test_case->run();
          foreach ($cli_results as $res) {
            $res->setName($res->getName() . " [CLI: $mname]");
          }
          $results[] = $cli_results;
        }
      }
    }

    foreach ($instances as $instance) {
      $results[] = $instance->generateValgrindTestResults();
    }

    // Also run the python tests if we built them
    foreach ($paths as $path) {
      if (!preg_match('/test.*\.py$/', $path)) {
        continue;
      }

      // build dir varies by platform, so just glob for it
      $pypath = implode(':', glob("python/bser/build/*"));

      // makefile contains the detected python, so just run the
      // rule from the makefile, but pass in our PYTHONPATH
      $start = microtime(true);
      $future = new ExecFuture(
        "PYTHONPATH=$pypath TESTNAME=$path make py-tests"
      );
      list($status, $out, $err) = $future->resolve();
      $end = microtime(true);
      $res = new ArcanistUnitTestResult();
      $res->setName($path);
      $res->setUserData($out.$err);
      $res->setDuration($end - $start);
      $res->setResult($status == 0 ?
        ArcanistUnitTestResult::RESULT_PASS :
        ArcanistUnitTestResult::RESULT_FAIL
      );
      $results[] = array($res);
    }

    $results = array_mergev($results);
    return $results;
  }

}

// vim:ts=2:sw=2:et:
