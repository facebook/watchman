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

class WatchmanTestCase extends ArcanistPhutilTestCase {
  protected $root;

  // because setProjectRoot is final and $this->projectRoot
  // is private...
  function setRoot($root) {
    $this->setProjectRoot($root);
    $this->root = $root;
  }

  function getRoot() {
    return $this->root;
  }

  function watchmanCommand() {
    $args = func_get_args();

    return call_user_func_array(
      array(WatchmanInstance::get(), 'request'),
      $args);
  }

  function assertRegex($pattern, $subject, $message = null) {
    if (!preg_match($pattern, $subject)) {
      if (!$message) {
        $message = "Failed to assert that $subject matches $pattern";
      }
      $this->assertFailure($message);
    }
  }

  function setLogLevel($level) {
    $out = WatchmanInstance::get()->setLogLevel($level);
    $this->assertEqual($level, $out['log_level'], "set log level to $level");
  }

  function waitForLog($criteria, $timeout = 5) {
    // Can't use the generic waitFor routine here because
    // we're delegating to a more efficient mechanism in
    // the instance class.
    return WatchmanInstance::get()->waitForLog($criteria, $timeout);
  }

  function assertWaitForLog($criteria, $timeout = 5) {
    list($ok, $line, $matches) = $this->waitForLog($criteria, $timeout);
    if (!$ok) {
      $this->assertFailure(
        "did not find $critiera in log output within $timeout seconds");
    }
    return array($ok, $line, $matches);
  }

  // Generic waiting assertion; continually invokes $callable
  // until timeout is hit.  Returns the returned value from
  // $callable if it is truthy.
  // Asserts failure if no truthy value is encountered within
  // the timeout
  function waitFor($callable, $timeout = 10, $message = null) {
    $deadline = time() + $timeout;
    while (time() <= $deadline) {
      $res = $callable();
      if ($res) {
        return $res;
      }
      usleep(30000);
    }
    if ($message === null) {
      $message = "Condition [$callable] was not met in $timeout seconds";
    }
    $this->assertFailure($message);
  }

  // Wait for a watchman command to return output that matches
  // some criteria.
  // Returns the command output.
  // $have_data is a callable that returns a boolean result
  // to indicate that the criteria have been met.
  // timeout is the timeout in seconds.
  function waitForWatchman(array $command, $have_data, $timeout = 10) {
    $cmd_text = implode(' ', $command);
    return $this->waitFor(
      function () use ($command, $have_data) {
        $out = call_user_func_array(
          array(WatchmanInstance::get(), 'request'),
          $command);
        if ($have_data($out)) {
          return $out;
        }
        return false;
      },
      $timeout,
      "watchman [$cmd_text] didn't yield results within $timeout seconds"
    );
  }
}


// vim:ts=2:sw=2:et:

