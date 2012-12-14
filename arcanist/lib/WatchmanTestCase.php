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
  function waitForNoThrow($callable, $timeout = 10) {
    $deadline = time() + $timeout;
    $res = null;
    while (time() <= $deadline) {
      $res = $callable();
      if ($res) {
        return array(true, $res);
      }
      usleep(30000);
    }
    return array(false, $res);
  }

  function waitFor($callable, $timeout = 10, $message = null) {
    list($ok, $res) = $this->waitForNoThrow($callable, $timeout);

    if ($ok) {
      return $res;
    }

    if ($message === null) {
      $message = "Condition [$callable] was not met in $timeout seconds";
    }
    if (is_callable($message)) {
      $message = $message();
    }
    $this->assertFailure($message);
  }

  // Wait for a watchman command to return output that matches
  // some criteria.
  // Returns the command output.
  // $have_data is a callable that returns a boolean result
  // to indicate that the criteria have been met.
  // timeout is the timeout in seconds.
  function waitForWatchmanNoThrow(array $command, $have_data, $timeout = 10) {
    $last_output = null;

    list($ok, $res) = $this->waitForNoThrow(
      function () use ($command, $have_data, &$last_output) {
        $out = call_user_func_array(
          array(WatchmanInstance::get(), 'request'),
          $command);
        $last_output = $out;
        if ($have_data($out)) {
          return $out;
        }
        return false;
      },
      $timeout
    );

    if ($ok) {
      return array(true, $res);
    }
    return array(false, $last_output);
  }

  function waitForWatchman(array $command, $have_data,
      $timeout = 10, $message = null)
  {
    list($ok, $res) = $this->waitForWatchmanNoThrow(
                        $command, $have_data, $timeout);
    if ($ok) {
      return $res;
    }

    if ($message === null) {
      $where = debug_backtrace();
      $where = array_shift($where);
      $where = sprintf("at line %d in file %s",
        idx($where, 'line'),
        basename(idx($where, 'file')));

      $cmd_text = implode(' ', $command);

      $message = "watchman [$cmd_text] didn't yield expected results " .
        "within $timeout seconds\n" . json_encode($last_output) . "\n" .
        $where;
    }

    $this->assertFailure($message);
  }

  function assertFileList($root, array $files, $timeout = 10, $message = null) {
    sort($files);

    $sort_func = function ($list) {
      $files = array();
      foreach ($list as $ent) {
        if ($ent['exists']) {
          $files[] = $ent['name'];
        }
      }
      sort($files);
      return $files;
    };

    list($ok, $out) = $this->waitForWatchmanNoThrow(
      array('find', $root),
      function ($out) use ($sort_func, $files) {
        return $sort_func(idx($out, 'files')) === $files;
      },
      $timeout
    );

    if ($ok) {
      return;
    }

    $got = $sort_func(idx($out, 'files'));

    if ($message === null) {
      $where = debug_backtrace();
      $where = array_shift($where);
      $where = sprintf("at line %d in file %s",
        idx($where, 'line'),
        basename(idx($where, 'file')));

      $message = "\nwatchman didn't yield expected file list " .
        "within $timeout seconds\n" . json_encode($out) . "\n" .
        $where;
    }

    $this->assertEqual($files, $got, $message);
  }
}


// vim:ts=2:sw=2:et:

