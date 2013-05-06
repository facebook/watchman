<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

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

  function waitForSub($subname, $callable, $timeout = 5) {
    return WatchmanInstance::get()->waitForSub($subname, $callable, $timeout);
  }

  function getSubData($subname) {
    return WatchmanInstance::get()->getSubData($subname);
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
        "did not find $criteria in log output within $timeout seconds");
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

      $cmd_text = implode(' ', $command);

      $message = "watchman [$cmd_text] didn't yield expected results " .
        "within $timeout seconds\n" . json_encode($res) . "\n" .
        $where;
    }

    $this->assertFailure($message);
  }

  function assertFileListUsingSince($root, $cursor, array $files,
      array $files_via_since = null, $timeout = 10, $message = null) {

    if ($cursor) {
      if ($files_via_since === null) {
        $files_via_since = $files;
      }
      sort($files_via_since);
    }

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
        return $sort_func(idx($out, 'files', array())) === $files;
      },
      $timeout
    );

    if ($ok) {

      if (!$cursor) {
        return;
      }

      $since = $this->watchmanCommand('since', $root, $cursor);

      $since_files = $sort_func(idx($since, 'files'));
      if ($since_files === $files_via_since) {
        return;
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
        "within $timeout seconds\n" . json_encode($out) . "\n" .
        $where;
    }

    $this->assertEqual($files, $got, $message);
  }

  function assertFileList($root, array $files, $timeout = 10,
        $message = null) {
    $this->assertFileListUsingSince($root, null, $files, null,
        $timeout, $message);
  }
}


// vim:ts=2:sw=2:et:

