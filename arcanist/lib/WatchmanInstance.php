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

// Keeps track of an instance of the watchman server
// for integration tests.
// Ensures that it is terminated when it is destroyed.
class WatchmanInstance {
  private $logfile;
  private $sockname;
  private $debug = false;
  private $sock;
  static $singleton = null;
  private $logdata = array();

  static function get() {
    if (!self::$singleton) {
      self::$singleton = new WatchmanInstance;
    }
    return self::$singleton;
  }

  function __construct() {
    $this->logfile = new TempFile();
    $this->sockname = new TempFile();
  }

  function setDebug($enable) {
    $this->debug = $enable;
  }

  protected function readResponses() {
    do {
      $data = fgets($this->sock);
      if ($data === false) return;
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
      stream_set_timeout($this->sock, 60);

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
    $f = new ExecFuture(
        "./watchman --sockname=%s.sock --logfile=%s",
        $this->sockname,
        $this->logfile);
    $f->resolve();

    $this->sock = fsockopen('unix://' . $this->sockname . '.sock');
    stream_set_timeout($this->sock, 60);
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

  function __destruct() {
    if ($this->sock) {
      $this->request('shutdown-server');
      if ($this->debug) {
        echo implode("", $this->logdata);
      }
    }
  }

}

// vim:ts=2:sw=2:et:

