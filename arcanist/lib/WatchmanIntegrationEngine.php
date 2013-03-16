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
    } else {
      $paths = $this->getPaths();
    }

    foreach ($paths as $path) {
      if (preg_match("/\.php$/", $path)) {
        require_once $path;
      }
    }

    // Take the startup cost of the binary out of the
    // individual test case costs
    WatchmanInstance::get()->request();

    // Find all the test cases that were declared
    $results = array();
    foreach (get_declared_classes() as $name) {
      $ref = new ReflectionClass($name);
      if (!$ref->isSubclassOf('WatchmanTestCase')) {
        continue;
      }

      // Good enough; let's use it
      $test_case = newv($name, array());
      $test_case->setRoot($root);
      $test_case->setPaths($paths);
      $results[] = $test_case->run();
    }

    $results[] = WatchmanInstance::get()->generateValgrindTestResults();

    $results = array_mergev($results);
    return $results;
  }

}

// vim:ts=2:sw=2:et:

