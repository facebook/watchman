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
  private $dir;
  private $logfile;
  private $sockname;
  private $invocations = 0;
  private $debug = false;
  private $sock;
  static $singleton = null;

  static function get() {
    if (!self::$singleton) {
      self::$singleton = new WatchmanInstance;
    }
    return self::$singleton;
  }

  function __construct() {
    $this->dir = realpath(dirname(__FILE__) . '/../../tests/integration');
    $this->logfile = $this->dir . "/.watchman.log";
    $this->sockname = $this->dir . "/.watchman.sock";
  }

  function setDebug($enable) {
    $this->debug = $enable;
  }

  function command() {
    $args = func_get_args();

    $args = array(
      "./watchman --sockname=%s --logfile=%s %Ls",
      $this->sockname,
      $this->logfile,
      $args);

    $this->invocations++;

    return newv('ExecFuture', $args);
  }

  function request() {
    $args = func_get_args();

    if (!$this->invocations) {
      return call_user_func_array(
        array($this, 'resolveCommand'),
        $args);
    }

    // Use a socket instead
    if (!$this->sock) {
      $this->sock = fsockopen('unix://' . $this->sockname);
    }

    fwrite($this->sock, json_encode($args));
    $data = fgets($this->sock);
    return json_decode($data, true);
  }

  function resolveCommand() {
    $args = func_get_args();

    $future = call_user_func_array(
      array($this, 'command'),
      $args);

    if ($this->debug) {
      echo "running: " . $future->getCommand() . "\n";
    }
    return $future->resolveJSON();
  }

  function __destruct() {
    if ($this->invocations) {
      $future = $this->command('shutdown-server');
      $future->resolvex();
    }
  }

}

// vim:ts=2:sw=2:et:

