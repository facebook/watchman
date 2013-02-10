<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Keeps track of an instance of the watchman server
// for integration tests.
// Ensures that it is terminated when it is destroyed.
class WatchmanInstance {
  private $proc;
  private $logfile;
  private $sockname;
  private $debug = false;
  private $valgrind = false;
  private $vg_log;
  private $sock;
  static $singleton = null;
  private $logdata = array();
  const TIMEOUT = 20;

  static function get() {
    if (!self::$singleton) {
      self::$singleton = new WatchmanInstance;
    }
    return self::$singleton;
  }

  function __construct() {
    $this->logfile = new TempFile();
    $this->sockname = new TempFile();

    if (getenv("WATCHMAN_VALGRIND")) {
      $this->valgrind = true;
      $this->vg_log = new TempFile();
    }
  }

  function setDebug($enable) {
    $this->debug = $enable;
  }

  protected function readResponses() {
    do {
      $data = fgets($this->sock);
      if ($data === false) {
        return false;
      }
      $resp = json_decode($data, true);
      if (!isset($resp['log'])) {
        return $resp;
      }
      // Collect log information
      $this->logdata[] = $resp['log'];
    } while (true);
  }

  function setLogLevel($level) {
    return $this->request('log-level', $level);
  }

  function waitForLog($criteria, $timeout = 5) {
    foreach ($this->logdata as $line) {
      $matches = array();
      if (preg_match($criteria, $line, $matches)) {
        return array(true, $line, $matches);
      }
    }

    $deadline = time() + $timeout;
    while (time() < $deadline) {
      stream_set_timeout($this->sock, $deadline - time());
      $data = fgets($this->sock);
      stream_set_timeout($this->sock, self::TIMEOUT);

      if ($data === false) {
        break;
      }
      $resp = json_decode($data, true);
      if (!isset($resp['log'])) {
        throw new Exception("expected a log response, got $data");
      }
      $this->logdata[] = $resp['log'];

      if (preg_match($criteria, $resp['log'], $matches)) {
        return array(true, $resp['log'], $matches);
      }
    }
    return array(false, null, null);
  }

  function start() {
    $cmd = "./watchman --foreground --sockname=%C.sock --logfile=%s " .
            "--statefile=%s.state --log-level=2";
    if ($this->valgrind) {
      $cmd = "valgrind --tool=memcheck " .
        "--log-file=$this->vg_log " .
        "--track-fds=yes " .
        "--read-var-info=yes " .
        "--track-origins=yes " .
        "--leak-check=full " .
        "--xml=yes " .
        "--xml-file=$this->vg_log.xml " .
        "-v " .
        $cmd;
    }

    $cmd = csprintf($cmd, $this->sockname, $this->logfile, $this->logfile);

    $pipes = array();
    $this->proc = proc_open($cmd, array(
      0 => array('file', '/dev/null', 'r'),
      1 => array('file', $this->logfile, 'a'),
      2 => array('file', $this->logfile, 'a'),
    ), $pipes);

    if (!$this->proc) {
      throw new Exception("Failed to spawn $cmd");
    }

    $sockname = $this->sockname . '.sock';
    $deadline = time() + 5;
    do {
      if (!file_exists($sockname)) {
        usleep(30000);
      }
      $this->sock = @fsockopen('unix://' . $sockname);
      if ($this->sock) {
        break;
      }
    } while (time() <= $deadline);
    if (!$this->sock) {
      throw new Exception("Failed to talk to watchman on $sockname");
    }
    stream_set_timeout($this->sock, self::TIMEOUT);
  }

  function request() {
    $args = func_get_args();

    if (!$this->sock) {
      $this->start();
    }

    if (!count($args)) {
      // No command to send right now (we're just warming up)
      return true;
    }

    $req = json_encode($args);
    if ($this->debug) {
      echo "Sending $req\n";
    }
    fwrite($this->sock, $req . "\n");
    return $this->readResponses();
  }

  private function renderVGStack($stack) {
    $text = '';
    foreach ($stack->frame as $frame) {
      if ($frame->file) {
        $origin = sprintf("%s:%d", $frame->file, $frame->line);
      } else {
        $origin = $frame->obj;
      }
      $text .= sprintf("\n  %40s   %s",
        $origin,
        $frame->fn);
    }
    return $text;
  }

  private function renderVGResult($err) {
    $text = "";
    if (isset($err->xwhat)) {
      $text .= (string)$err->xwhat->text;
    }
    if (isset($err->what)) {
      if (strlen($text)) $text .= "\n";
      $text .= (string)$err->what;
    }
    if (isset($err->auxwhat)) {
      if (strlen($text)) $text .= "\n";
      $text .= (string)$err->auxwhat;
    }
    $text .= $this->renderVGStack($err->stack);

    return $text;
  }

  function generateValgrindTestResults() {
    $this->stopProcess();

    if (!$this->valgrind) {
      return array();
    }

    $definite_leaks = array();
    $possible_leaks = array();
    $errors = array();
    $descriptors = array();

    // valgrind seems to use an interesting definition of valid XML.
    // Tolerate having multiple documents in one file.
    $xml_data = file_get_contents($this->vg_log . '.xml');
    preg_match_all(',<valgrindoutput>.*</valgrindoutput>,Usm',
      $xml_data, $matches);
    foreach ($matches[0] as $data) {
      $vg = simplexml_load_string($data);
      if (is_object($vg)) {
        foreach ($vg->error as $err) {
          $render = $this->renderVGResult($err);
          switch ($err->kind) {
          case 'Leak_DefinitelyLost':
            $definite_leaks[] = $render;
            break;
          case 'Leak_PossiblyLost':
            $possible_leaks[] = $render;
            break;
          default:
            $errors[] = $render;
          }
        }
      } else {
        var_dump($xml_data);
      }
    }

    // These look like fd leak records, but they're not documented
    // as such.  These go away if we turn off track-fds
    foreach ($vg->stack as $stack) {
      $descriptors[] = $this->renderVGStack($stack);
    }

    $results = array();

    $res = new ArcanistUnitTestResult();
    $res->setName('valgrind possible leaks');
    $res->setUserData(implode("\n\n", $possible_leaks));
    $res->setResult(count($possible_leaks) ?
      ArcanistUnitTestResult::RESULT_SKIP :
      ArcanistUnitTestResult::RESULT_PASS);
    $results[] = $res;

    $res = new ArcanistUnitTestResult();
    $res->setName('descriptor leaks');
    $res->setUserData(implode("\n\n", $descriptors));
    $res->setResult(count($descriptors) ?
      ArcanistUnitTestResult::RESULT_FAIL :
      ArcanistUnitTestResult::RESULT_PASS);
    $results[] = $res;

    $res = new ArcanistUnitTestResult();
    $res->setName('valgrind leaks');
    $res->setUserData(implode("\n\n", $definite_leaks));
    $res->setResult(count($definite_leaks) ?
      ArcanistUnitTestResult::RESULT_FAIL :
      ArcanistUnitTestResult::RESULT_PASS);
    $results[] = $res;

    $res = new ArcanistUnitTestResult();
    $res->setName('valgrind errors');
    $res->setUserData(implode("\n\n", $errors));
    $res->setResult(count($errors) ?
      ArcanistUnitTestResult::RESULT_FAIL :
      ArcanistUnitTestResult::RESULT_PASS);
    $results[] = $res;


    return $results;
  }

  private function waitForStop($timeout) {
    if (!$this->proc) {
      return false;
    }

    $deadline = time() + $timeout;
    do {
      $st = proc_get_status($this->proc);
      if (!$st['running']) {
        return $st;
      }
      usleep(30000);
    } while (time() <= $deadline);

    return $st;
  }

  function stopProcess() {
    if (!$this->proc) {
      return;
    }
    if ($this->sock) {
      $this->request('shutdown-server');
      $st = $this->waitForStop(5);
    } else {
      $st = proc_get_status($this->proc);
    }

    if ($st['running']) {
      echo "Didn't stop after 5 seconds, sending signal\n";
      system("gstack " . $st['pid']);
      proc_terminate($this->proc);
      $st = $this->waitForStop(5);
      if ($st['running']) {
        echo "Still didn't stop, sending bigger signal\n";
        proc_terminate($this->proc, 9);
        $st = $this->waitForStop(5);
      }
    }
    if ($st['running']) {
      echo "Why won't you die!???\n";
      var_dump($st);
    }
    $this->proc = null;
    if ($this->debug) {
      readfile($this->logfile);
    }
    copy($this->logfile, '/tmp/watchman-test.log');
  }

  function __destruct() {
    $this->stopProcess();
  }

}

// vim:ts=2:sw=2:et:

