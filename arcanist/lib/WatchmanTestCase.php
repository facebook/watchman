<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

if (!defined('PHP_BINARY')) {
  define('PHP_BINARY', 'php');
}

function w_unlink($name) {
  if (phutil_is_windows()) {
    for ($i = 0; $i < 10; $i++) {
      $x = @unlink($name);
      if ($x) return true;
      usleep(200000);
    }
  }
  return unlink($name);
}

function w_normalize_filename($a) {
  if ($a === null) {
    return null;
  }
  $a = str_replace('\\', '/', $a);
  return $a;
}

function w_is_same_filename($a, $b) {
  return w_normalize_filename($a) == w_normalize_filename($b);
}

function w_is_file_in_file_list($filename, $list) {
  $list = w_normalize_file_list($list);
  $filename = w_normalize_filename($filename);
  return in_array($filename, $list);
}

function w_normalize_file_list($a) {
  return array_map('w_normalize_filename', $a);
}

function w_is_same_file_list($a, $b) {
  $a = w_normalize_file_list($a);
  $b = w_normalize_file_list($b);
  return $a == $b;
}

function w_find_subdata_containing_file($subdata, $filename) {
  if (!is_array($subdata)) {
    return null;
  }
  $filename = w_normalize_filename($filename);
  foreach ($subdata as $sub) {
    if (in_array($filename, $sub['files'])) {
      return $sub;
    }
  }
  return null;
}

class TestSkipException extends Exception {}

class WatchmanTestCase {
  protected $root;
  protected $watchman_instance;
  private $use_cli = false;
  private $cli_args = null;
  private $watches = array();
  static $test_number = 1;

  // If this returns false, we can run this test case using
  // the CLI instead of via a unix socket
  function needsLiveConnection() {
    return false;
  }

  function isUsingCLI() {
    return $this->use_cli;
  }

  function useCLI($args) {
    $this->use_cli = true;
    $this->cli_args = $args;
  }

  function setRoot($root) {
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

  function watchProject($root) {
    $res = $this->watchmanCommand('watch-project', $root);
    if (!is_array($res)) {
      $err = $res;
    } else {
      $err = idx($res, 'error');
    }
    if (!$err) {
      // Remember the watched dir
      $this->watches[$root] = idx($res, 'watch');
    }
    return $res;
  }

  function watch($root, $assert = true) {
    $root = w_normalize_filename($root);
    $res = $this->watchmanCommand('watch', $root);
    $this->watches[$root] = $res;
    if ($assert) {
      if (!is_array($res)) {
        $err = $res;
      } else {
        $err = idx($res, 'error', w_normalize_filename(idx($res, 'watch')));
      }
      $this->assertEqual(w_normalize_filename($root), $err);
    }
    return $res;
  }

  function trigger() {
    $args = func_get_args();
    array_unshift($args, 'trigger');
    if (is_string($args[2])) {
      $id = $args[2];
    } else {
      $id = $args[2]['name'];
    }
    $out = call_user_func_array(array($this, 'watchmanCommand'), $args);
    if (!is_array($out)) {
      $err = $out;
    } else {
      $err = idx($out, 'error', idx($out, 'triggerid'), 'unpossible');
    }
    $def = json_encode($args);
    $output = json_encode($out);
    $message = "trigger definition: $def, output was $output";
    $this->assertEqual($id, $err, $message);
    return $out;
  }

  private function computeWatchmanTestCaseName($test_method_name = '') {
    $cls = get_class($this);
    if ($test_method_name) {
      $cls .= "::$test_method_name";
    }
    return $cls;
  }

  private function logTestInfo($msg, $test_method_name = '') {
    try {
      $name = $this->computeWatchmanTestCaseName($test_method_name);
      $this->watchmanCommand(
        'log',
        'debug',
        "TEST: $msg $name\n\n"
      );
    } catch (Exception $e) {
      printf(
        "logTestInfo %s %s failed: %s\n",
        $msg,
        $name,
        $e->getMessage()
      );
    }
  }

  function didRunOneTest($test_method_name) {
    if (!$this->use_cli) {
      $this->watchman_instance->stopLogging();
    }
    chdir($this->root);
    $this->logTestInfo('end', $test_method_name);
  }

  function willRunOneTest($test_method_name) {
    chdir($this->root);
    $this->logTestInfo('begin', $test_method_name);
  }

  function willRunTests() {
    $this->logTestInfo('willRun');
  }

  function didRunTests() {
    $this->logTestInfo('didRun');

    try {
      $this->watchmanCommand('watch-del-all');
    } catch (Exception $e) {
      // Swallow
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

  function waitForSub($subname, $callable, $timeout = 15) {
    return $this->watchman_instance->waitForSub($subname, $callable, $timeout);
  }

  function getSubData($subname) {
    return $this->watchman_instance->getSubData($subname);
  }

  function waitForLog($criteria, $timeout = 15) {
    $this->assertLiveConnection();
    // Can't use the generic waitFor routine here because
    // we're delegating to a more efficient mechanism in
    // the instance class.
    return $this->watchman_instance->waitForLog($criteria, $timeout);
  }

  function assertWaitForLog($criteria, $timeout = 15) {
    list($ok, $line, $matches) = $this->waitForLog($criteria, $timeout);
    if (!$ok) {
      $this->assertFailure(
        "did not find $criteria in log output within $timeout seconds");
    }
    return array($ok, $line, $matches);
  }

  function waitForLogOutput($criteria, $timeout = 15) {
    // Can't use the generic waitFor routine here because
    // we're delegating to a more efficient mechanism in
    // the instance class.
    return $this->watchman_instance->waitForLogOutput($criteria, $timeout);
  }

  function assertWaitForLogOutput($criteria, $timeout = 15) {
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
  function waitForNoThrow($callable, $timeout = 20) {
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

  function waitFor($callable, $timeout = 20, $message = null) {
    list($ok, $res) = $this->waitForNoThrow($callable, $timeout);

    if ($ok) {
      return $res;
    }

    if ($message === null) {
      $message = "Condition [$callable] was not met in $timeout seconds";
    }
    if (is_callable($message)) {
      $message = call_user_func($message);
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
  function waitForWatchmanNoThrow(array $command, $have_data, $timeout = 20) {
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
      $timeout = 20, $message = null)
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
        return w_is_same_file_list(
          $sort_func(idx($out, 'files', array())), $files);
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
      if (w_is_same_file_list($since_files, $files_via_since)) {
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

    $this->assertEqualFileList($files, $got, $message);
  }

  function assertEqualFileList($a, $b, $message = null) {
    if ($message === null) {
      $where = debug_backtrace();
      $where = array_shift($where);
      $where = sprintf("at line %d in file %s",
        idx($where, 'line'),
        basename(idx($where, 'file')));

      $message = "\nfile lists are not equal $where";
    }
    $a = w_normalize_file_list($a);
    sort($a);
    $b = w_normalize_file_list($b);
    sort($b);
    $this->assertEqual($a, $b, $message . " " . json_encode($a) .
                               " vs " .json_encode($b));
  }

  function assertFileList($root, array $files, $message = null) {
    $this->assertFileListUsingSince($root, null, $files, null, $message);
  }

   function secondLevelSort(array $objs) {
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

  function waitForFileContents($filename, $content, $timeout = 15) {
    $this->waitFor(
      function () use ($filename, $content) {
        $got = @file_get_contents($filename);
        return $got === $content;
      },
      $timeout,
      function () use ($filename, $content) {
        $got = @file_get_contents($filename);
        return "Wanted: $content\nGot:    $got\n".
               "wait for $filename to hold a certain content";
      }
    );
    return @file_get_contents($filename);
  }

  function assertFileContents($filename, $content, $timeout = 15) {
    $got = $this->waitForFileContents($filename, $content, $timeout);
    $this->assertEqual($got, $content,
        "waiting for $filename to have a certain content");
  }

  function waitForFileToHaveNLines($filename, $nlines, $timeout = 15) {
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

  function waitForJsonInput($log, $timeout = 15) {
    $this->waitFor(
      function () use ($log) {
        $data = @file_get_contents($log);
        if (!strlen($data)) {
          return false;
        }
        $obj = @json_decode(trim($data), true);
        return is_array($obj);
      },
      $timeout,
      "waiting for $log to hold a JSON object"
    );

    $obj = json_decode(trim(file_get_contents($log)), true);
    $this->assertTrue(is_array($obj), "got JSON object in $log");

    return $obj;
  }

  function isCaseInsensitive() {
    static $insensitive = null;
    if ($insensitive === null) {
      $dir = new WatchmanDirectoryFixture();
      $path = $dir->getPath();
      touch("$path/a");
      $insensitive = file_exists("$path/A");
    }
    return $insensitive;
  }

  function run() {
    $ref = new ReflectionClass($this);
    $methods = $ref->getMethods();
    shuffle($methods);
    $this->willRunTests();
    foreach ($methods as $method) {
      $name = $method->getName();
      if (!preg_match('/^test/', $name)) {
        continue;
      }

      try {
        $this->willRunOneTest($name);

        call_user_func(array($this, $name));

        try {
          $this->didRunOneTest($name);
        } catch (Exception $e) {
          $this->failException($e);
        }

      } catch (TestSkipException $e) {
        // Continue with next
      } catch (Exception $e) {
        $this->failException($e);
      }
    }
    $this->didRunTests();
  }

  function failException($e) {
    $this->fail(sprintf("%s: %s\n%s",
      get_class($e),
      $e->getMessage(),
      $e->getTraceAsString()));
  }

  function printStatus($ok, $message) {
    $lines = explode("\n", $message);
    if (count($lines) > 1) {
      echo '# ' . implode("\n# ", $lines) . "\n";
    }
    $last_line = array_pop($lines);
    $caller = self::getCallerInfo();

    printf("%s %d - %s:%d: %s\n",
      $ok ? 'ok' : 'not ok',
      self::$test_number++,
      $caller['file'],
      $caller['line'],
      $last_line);
  }

  function fail($message) {
    $this->printStatus(false, $message);
    throw new TestSkipException();
  }

  function ok($message) {
    $this->printStatus(true, $message);
  }


  /**
   * Returns info about the caller function.
   *
   * @return map
   */
  private static final function getCallerInfo() {
    $caller = array();

    foreach (array_slice(debug_backtrace(), 1) as $location) {
      $function = idx($location, 'function');

      if (idx($location, 'file') == __FILE__) {
        continue;
      }
      $caller = $location;
      break;
    }

    return array(
      'file' => basename(idx($caller, 'file')),
      'line' => idx($caller, 'line'),
    );
  }

  static function printable($value) {
    return json_encode($value);
  }

  function assertEqual($expected, $actual, $message = null) {
    if ($message === null) {
      $message = sprintf("Expected %s to equal %s",
        self::printable($actual),
        self::printable($expected));
    }
    if ($expected === $actual) {
      $this->ok($message);
    } else {
      $this->fail($message);
    }
  }

  function assertTrue($actual, $message = null) {
    $this->assertEqual(true, $actual, $message);
  }

  function assertFalse($actual, $message = null) {
    $this->assertEqual(false, $actual, $message);
  }

  function assertFailure($message) {
    return $this->fail($message);
  }

  function assertSkipped($message) {
    $this->ok("skip: $message");
    throw new TestSkipException();
  }
}

// vim:ts=2:sw=2:et:
