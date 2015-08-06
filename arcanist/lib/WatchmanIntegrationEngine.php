<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Runs both the integration and the unit tests
class WatchmanIntegrationEngine extends WatchmanTapEngine {
  var $first_inst;

  public function setWatchmanInstance($inst) {
    $this->first_inst = $inst;
  }

  public function run($tests) {
    return $this->runIntegrationTests($tests);
  }

  public function runIntegrationTests($tests) {
    // Now find all the test programs
    $root = $this->getProjectRoot();
    $test_dir = $root . "/tests/integration/";

    if (!$tests) {
      $paths = glob($test_dir . "*.php");
      $paths[] = 'ruby/ruby-watchman/spec/ruby_watchman_spec.rb';
    } else {
      $paths = $tests;
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
        // Don't pull in files starting with "_"; we're using
        // those as helpers for triggers
        $base = basename($path);
        if ($base[0] != '_') {
          require_once $path;
        }
      }
    }

    // We test for this in a test case
    putenv("WATCHMAN_EMPTY_ENV_VAR=");

    $coverage = $this->getEnableCoverage();

    if (!$this->first_inst) {
      $this->first_inst = new WatchmanInstance($root, $coverage);
    }
    $first_inst = $this->first_inst;
    $instances = array($first_inst);

    // Helper for python or other language tests
    putenv("WATCHMAN_SOCK=".$first_inst->getFullSockName());

    // Exercise the different serialization combinations
    $cli_matrix = array(
#      'bser/json' => '--server-encoding=bser --output-encoding=json',
#      'json/json' => '--server-encoding=json --output-encoding=json',
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
        $instance = $first_inst;
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

    foreach ($paths as $path) {
      if (!preg_match('/\.rb$/', $path)) {
        continue;
      }
      if (!file_exists($path)) {
        // Was deleted in this (pending) rev
        continue;
      }
      $start = microtime(true);
      $future = new ExecFuture(
        "PATH=\"$root:\$PATH\" \${MAKE:-make} rb-tests"
      );
      $future->setTimeout(10);
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

    foreach ($instances as $instance) {
      $results[] = $instance->generateValgrindTestResults();
    }

    $results = array_mergev($results);
    return $results;
  }

}

// vim:ts=2:sw=2:et:
