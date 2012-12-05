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

    foreach (glob($test_dir . "*.php") as $test) {
      require_once $test;
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
      $test_case->setPaths($this->getPaths());
      $results[] = $test_case->run();
    }

    $results = array_mergev($results);
    return $results;
  }

}

// vim:ts=2:sw=2:et:

