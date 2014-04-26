<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class WatchmanTestCase extends ArcanistPhutilTestCase {
  protected $root;
  protected $watchman_instance;
  private $use_cli = false;
  private $cli_args = null;
  private $watches = array();

  // If this returns false, we can run this test case using
  // the CLI instead of via a unix socket
  function needsLiveConnection() {
    return false;
  }

  function useCLI($args) {
    $this->use_cli = true;
    $this->cli_args = $args;
  }

  // because setProjectRoot is final and $this->projectRoot
  // is private...
  function setRoot($root) {
    $this->setProjectRoot($root);
    $this->root = $root;
  }

  function getRoot() {
    return $this->root;
  }

  // This can be overridden if your test requires specific global config options
  function getGlobalConfig() {
    return array();
  }

  function setWatchmanInstance($instance) {
    $this->watchman_instance = $instance;
  }

  function watch($root) {
    $res = $this->watchmanCommand('watch', $root);
    $this->watches[$root] = $res;
    return $res;
  }

  function didRunOneTest($test_method_name) {
    if (!$this->use_cli) {
      $this->watchman_instance->stopLogging();
    }
  }

  function didRunTests() {
    foreach ($this->watches as $root => $status) {
      try {
        $this->watchmanCommand('watch-del', $root);
      } catch (Exception $e) {
        // Swallow
      }
    }
    $this->watches = array();
  }

  function watchmanCommand() {
    $args = func_get_args();

    if ($this->use_cli) {
      $future = new WatchmanQueryFuture(
        $this->watchman_instance->getFullSockName(),
        $this->root,
        $this->cli_args,
        $args
      );
      return $future->resolve();
    }

    return call_user_func_array(
      array($this->watchman_instance, 'request'),
      $args);
  }

  function assertRegex($pattern, $subject, $message = null) {
    if (!$message) {
      $message = "Failed to assert that $subject matches $pattern";
    }
    $this->assertTrue(preg_match($pattern, $subject) === 1, $message);
  }

  /**
   * Suspends the watchman process.
   *
   * This is useful when testing to try to force batching or coalescing in the
   * kernel notification layer.  You must have a matching resumeProcess() call.
   */
  function suspendWatchman() {
    $this->watchman_instance->suspendProcess();
  }

  /**
   * Resumes the watchman process. This is meant to be called while the watchman
   * process is suspended.
   */
  function resumeWatchman() {
    $this->watchman_instance->resumeProcess();
  }

  function assertLiveConnection() {
    $this->assertTrue(
      $this->needsLiveConnection(),
      "you must override needsLiveConnection and make it return true"
    );
  }

  function startLogging($level) {
    $this->assertLiveConnection();
    $out = $this->watchman_instance->startLogging($level);
    $this->assertEqual($level, $out['log_level'], "set log level to $level");
  }

  function stopLogging() {
    $this->assertLiveConnection();
    $out = $this->watchman_instance->stopLogging();
    $this->assertEqual('off', $out['log_level'], "set log level to 'off'");
  }

  function waitForSub($subname, $callable, $timeout = 5) {
    return $this->watchman_instance->waitForSub($subname, $callable, $timeout);
  }

  function getSubData($subname) {
    return $this->watchman_instance->getSubData($subname);
  }

  function waitForLog($criteria, $timeout = 5) {
    $this->assertLiveConnection();
    // Can't use the generic waitFor routine here because
    // we're delegating to a more efficient mechanism in
    // the instance class.
    return $this->watchman_instance->waitForLog($criteria, $timeout);
  }

  function assertWaitForLog($criteria, $timeout = 5) {
    list($ok, $line, $matches) = $this->waitForLog($criteria, $timeout);
    if (!$ok) {
      $this->assertFailure(
        "did not find $criteria in log output within $timeout seconds");
    }
    return array($ok, $line, $matches);
  }

  function waitForLogOutput($criteria, $timeout = 5) {
    // Can't use the generic waitFor routine here because
    // we're delegating to a more efficient mechanism in
    // the instance class.
    return $this->watchman_instance->waitForLogOutput($criteria, $timeout);
  }

  function assertWaitForLogOutput($criteria, $timeout = 5) {
    list($ok, $line, $matches) = $this->waitForLogOutput($criteria, $timeout);
    if (!$ok) {
      $this->assertFailure(
        "did not find $criteria in log file within $timeout seconds");
    }
    return array($ok, $line, $matches);
  }

  // Generic waiting assertion; continually invokes $callable
  // until timeout is hit.  Returns the returned value from
  // $callable if it is truthy.
  // Asserts failure if no truthy value is encountered within
  // the timeout
  function waitForNoThrow($callable, $timeout = 10) {
    $current_time = time();
    $deadline = $current_time + $timeout;
    $res = null;
    do {
      try {
        $res = $callable();
        if ($res) {
          return array(true, $res);
        }
      } catch (Exception $e) {
        $res = $e->getMessage();
        break;
      }
      usleep(30000);
      $current_time = time();
    } while ($current_time < $deadline);
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
    if (is_string($res)) {
      $message .= " $res";
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

    $instance = $this->watchman_instance;
    list($ok, $res) = $this->waitForNoThrow(
      function () use ($instance, $command, $have_data, &$last_output) {
        $out = call_user_func_array(
          array($instance, 'request'),
          $command);
        if ($out === false) {
          // Connection terminated
          $last_output = "watchman went away";
          throw new Exception($last_output);
        }
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

      $cmd_text = json_encode($command);

      $message = "watchman [$cmd_text] didn't yield expected results " .
        "within $timeout seconds\n" . json_encode($res) . "\n" .
        $where;
    }

    $this->assertFailure($message);
  }

  function assertFileListUsingSince($root, $cursor, array $files,
      array $files_via_since = null, $message = null) {

    if ($cursor) {
      if ($files_via_since === null) {
        $files_via_since = $files;
      }
      sort($files_via_since);
    }

    sort($files);

    $sort_func = function ($list) {
      $files = array();
      if (!is_array($list)) {
        return $files;
      }
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
        return $sort_func(idx($out, 'files', array())) === $files;
      },
      0 // timeout
    );

    if ($ok) {

      if (!$cursor) {
        // we've already gotten all the files we care about
        $this->assertTrue(true);
        return;
      }

      $since = $this->watchmanCommand('since', $root, $cursor);

      $since_files = $sort_func(idx($since, 'files'));
      if ($since_files === $files_via_since) {
        $this->assertTrue(true);
        return $since;
      }

      if ($message === null) {
        $where = debug_backtrace();
        $where = array_shift($where);
        $where = sprintf("at line %d in file %s",
          idx($where, 'line'),
          basename(idx($where, 'file')));

        $message = "\nwatchman since vs. find result mismatch\n" .
          json_encode($out) . "\n" .
          json_encode($since) . "\n" .
          $where;

        $message .= "\nsince_files = " . json_encode($since_files) .
                    "\ngot_files = " . json_encode($files) . "\n";

      }

      $got = $since_files;
    } elseif (is_array($out)) {
      $error = idx($out, 'error');
      if ($error) {
        throw new Exception($error);
      }
      $got = $sort_func(idx($out, 'files'));
    } else {
      $got = $out;
    }

    if ($message === null) {
      $where = debug_backtrace();
      $where = array_shift($where);
      $where = sprintf("at line %d in file %s",
        idx($where, 'line'),
        basename(idx($where, 'file')));

      $message = "\nwatchman didn't yield expected file list " .
        json_encode($out) . "\n" . $where;
    }

    $this->assertEqual($files, $got, $message);
  }

  function assertFileList($root, array $files, $message = null) {
    $this->assertFileListUsingSince($root, null, $files, null, $message);
  }

  private function secondLevelSort(array $objs) {
    $ret = array();

    foreach ($objs as $obj) {
      ksort($obj);
      $ret[] = $obj;
    }

    return $ret;
  }

  function assertTriggerList($root, $trig_list) {
    $triggers = $this->watchmanCommand('trigger-list', $root);

    $triggers = $triggers['triggers'];
    usort($triggers, function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual(
      $this->secondLevelSort($trig_list),
      $this->secondLevelSort($triggers)
    );
  }

  function waitForFileContents($filename, $content, $timeout = 5) {
    $this->waitFor(
      function () use ($filename, $content) {
        $got = @file_get_contents($filename);
        return $got === $content;
      },
      $timeout,
      function () use ($filename, $content) {
        $got = @file_get_contents($filename);
        return "wait for $filename to hold $content, got $got";
      }
    );
    return @file_get_contents($filename);
  }

  function assertFileContents($filename, $content, $timeout = 5) {
    $got = $this->waitForFileContents($filename, $content, $timeout);
    $this->assertEqual($got, $content);
  }

  function waitForFileToHaveNLines($filename, $nlines, $timeout = 5) {
    $this->waitFor(
      function () use ($filename, $nlines) {
        return count(@file($filename)) == $nlines;
      },
      $timeout,
      function () use ($filename, $nlines) {
        $lines = count(@file($filename));
        return "wait for $filename to hold $nlines lines, got $lines";
      }
    );
    return @file($filename, FILE_IGNORE_NEW_LINES);
  }

  function waitForJsonInput($log, $timeout = 5) {
    $this->waitFor(
      function () use ($log) {
        $data = @file_get_contents($log);
        if (!strlen($data)) {
          return false;
        }
        $obj = @json_decode($data, true);
        return is_array($obj);
      },
      $timeout,
      "waiting for $log to hold a JSON object"
    );

    $obj = json_decode(file_get_contents($log), true);
    $this->assertTrue(is_array($obj), "got JSON object in $log");

    return $obj;
  }


}

// vim:ts=2:sw=2:et:
