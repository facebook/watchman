<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

/**
 * Returns a Future instance that will resolve to the
 * parsed output of a Watchman file query; use the
 * resolve method of the returned future to get
 * at the response.
 *
 * This delegates to the watchman command line utility.
 */
class WatchmanQueryFuture extends FutureProxy {
  private $command;

  public function __construct($sockname, $repo_root, $args, array $command) {
    $this->command = json_encode($command);
    $console = PhutilConsole::getConsole();
    $console->writeLog(
      "cli query: %s\n",
      $this->command
    );
    $future = new ExecFuture(
      "%C $args -U %s %s %s %s %s",
      "$repo_root/watchman",
      $sockname,
      '--logfile=/BOGUS',
      '--statefile=/BOGUS',
      '--no-pretty',
      '--no-spawn',
      '--no-local',
      '-j'
    );

    $cwd = getcwd();
    if (!$cwd) {
      throw new Exception("can't figure out my cwd!?");
    }
    $future->setCWD($cwd);
    $future->write($this->command . "\n");
    parent::__construct($future);
  }

  // For some reason, FutureProxy redefines this to call the
  // proxy directly, instead of routing via isReady().
  // Reinstate that so we have a consistent way to track whether
  // we're started
  public function start() {
    $this->isReady();
    return $this;
  }

  // We also need to force resolve through the same path too
  public function resolve($timeout = null) {
    $this->isReady();
    return parent::resolve($timeout);
  }

  public function isReady() {
    return $this->getProxiedFuture()->isReady();
  }

  protected function didReceiveResult($result) {
    list($err, $out, $stderr) = $result;

    $decoded = json_decode($out, true);

    if (!is_array($decoded)) {
      throw new Exception(
        "watchman query $this->command did not produce a ".
        "valid JSON object as its output:\n".
        json_encode($result)
      );
    }

    if (!isset($decoded['version'])) {
      throw new Exception(
        "watchman query $this->command did not produce a ".
        "valid response as its output:\n".
        json_encode($result)
      );
    }

    return $decoded;
  }

  public function setTimeout($timeout) {
    $this->getProxiedFuture()->setTimeout($timeout);
  }
}
