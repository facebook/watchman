<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Keeps track of an instance of the watchman server
// for integration tests.
// Ensures that it is terminated when it is destroyed.
class WatchmanInstance {
  protected $proc;
  protected $logfile;
  protected $sockname;
  protected $config_file;
  protected $debug = false;
  protected $valgrind = false;
  protected $coverage = false;
  protected $vg_log;
  protected $cg_file;
  protected $sock;
  protected $repo_root;
  protected $logdata = array();
  protected $subdata = array();
  const TIMEOUT = 20;

  private function tempfile() {
    $temp = tempnam(sys_get_temp_dir(), 'wat');
    return $temp;
  }

  function __construct($repo_root, $coverage, $config = array()) {
    $this->repo_root = $repo_root;
    $this->logfile = $this->tempfile();
    if (phutil_is_windows()) {
      $this->sockname = "\\\\.\\pipe\\watchman-test-" . uniqid();
    } else {
      $this->sockname = $this->tempfile();
    }
    $this->config_file = $this->tempfile();
    // PHP is incredibly stupid: there's no direct way to turn array() into '{}'
    // and array('foo' => array('bar', 'baz')) into '{"foo": ["bar", "baz"]}'.
    if ($config === array()) {
      $config_json = '{}';
    } else {
      $config_json = json_encode($config);
    }
    file_put_contents($this->config_file, $config_json);

    if (getenv("WATCHMAN_VALGRIND")) {
      $this->valgrind = true;
      $this->vg_log = $this->tempfile();
    } elseif ($coverage) {
      $this->coverage = true;
      $this->cg_file = $this->tempfile();
    }

    $this->request();
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
      if (isset($resp['log'])) {
        // Collect log information
        $this->logdata[] = $resp['log'];
        continue;
      }
      if (isset($resp['subscription'])) {
        // Collect subscription information
        $name = $resp['subscription'];
        if (!isset($this->subdata[$name])) {
          $this->subdata[$name] = array();
        }
        $this->subdata[$name][] = $resp;
        continue;
      }

      return $resp;
    } while (true);
  }

  function waitForSub($subname, $callable, $timeout = 5) {
    if (isset($this->subdata[$subname])) {
      if ($callable($this->subdata[$subname])) {
        return $this->subdata[$subname];
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
      $name = idx($resp, 'subscription');
      if (!$name) {
        throw new Exception("expected a subscription response, got $data");
      }
      $this->subdata[$name][] = $resp;

      if ($name == $subname) {
        if ($callable($this->subdata[$subname])) {
          return $this->subdata[$subname];
        }
      }
    }
    return array();
  }

  function getProcessID() {
    try {
      $resp = $this->request('get-pid');
      return idx($resp, 'pid');
    } catch (Exception $e) {
      printf("get-pid: %s\n", $e->getMessage());
      return null;
    }
  }

  // Looks in the log file for the matching term.
  // This is useful for checking stdout/stderr from triggers
  function waitForLogOutput($criteria, $timeout = 5) {
    $deadline = time() + $timeout;
    while (time() < $deadline) {
      foreach (file($this->logfile.'.log') as $line) {
        if (preg_match($criteria, $line, $matches)) {
          return array(true, $line, $matches);
        }
      }
      usleep(200);
    }
    return array(false, null, null);
  }

  /** Get and clear data we collected for a subscription */
  function getSubData($subname) {
    $data = idx($this->subdata, $subname);
    unset($this->subdata[$subname]);
    return $data;
  }

  function getLogData() {
    return $this->logdata;
  }

  // Looks in json stream for logging output
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

  function getFullSockName() {
    if (phutil_is_windows()) {
      return $this->sockname;
    }
    return $this->sockname . '.sock';
  }

  function start() {
    $cmd = "%s --foreground --sockname=%s --logfile=%s.log " .
            "--statefile=%s.state --log-level=2 --pidfile=%s.pid";
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
    } else if ($this->coverage) {
      $cmd = "valgrind --tool=callgrind --collect-jumps=yes " .
        "--separate-recs=16 --callgrind-out-file=$this->cg_file " .
        $cmd;
    }

    putenv("WATCHMAN_CONFIG_FILE=".$this->config_file);

    $watchman_bin = $this->repo_root . '/watchman';
    if (!file_exists($watchman_bin)) {
      // Probably inside a buck-built test suite.  We "know" that
      // the test running machinery has set WATCHMAN_BINARY to the
      // appropriate path, but if not, we just have to assume that
      // it is in the PATH and trust that.
      $watchman_bin = idx($_ENV, 'WATCHMAN_BINARY', 'watchman');
    }

    $cmd = sprintf($cmd, $watchman_bin,
      $this->getFullSockName(), $this->logfile,
      $this->logfile, $this->logfile);

    $pipes = array();
    $this->proc = proc_open($cmd, array(
      0 => array('file', phutil_is_windows() ? 'NUL:' : '/dev/null', 'r'),
      1 => array('file', $this->logfile, 'a'),
      2 => array('file', $this->logfile, 'a'),
    ), $pipes, $this->repo_root);

    if (!$this->proc) {
      throw new Exception("Failed to spawn $cmd");
    }

    $this->openSock();

    // If you're debugging and want to attach a debugger, then:
    // `WATCHMAN_DEBUG_WAIT=1 arc test tests/integration/age.php`
    // then gdb -p or lldb -p with the PID it prints out
    if (getenv("WATCHMAN_DEBUG_WAIT")) {
      printf("PID: %d\n", $this->getProcessID());
      sleep(10);
    }
  }

  function openSock() {
    $sockname = $this->getFullSockName();
    $timeout = 5;
    $deadline = time() + $timeout;
    do {
      if (phutil_is_windows()) {
        $this->sock = @fopen($this->sockname, 'r+');
        if (!$this->sock) {
          usleep(30000);
        }
      } else {
        if (!file_exists($sockname)) {
          usleep(30000);
        }

        $this->sock = @fsockopen('unix://' . $sockname);
      }
      if ($this->sock) {
        break;
      }
    } while (time() <= $deadline);
    if (!$this->sock) {
      throw new Exception("Failed to talk to watchman within ".
        "$timeout seconds on $sockname");
    }
    stream_set_timeout($this->sock, self::TIMEOUT);
  }

  private function fwrite_all($sock, $buf) {
    $wrote = 0;
    for ($total = 0; $total < strlen($buf); $total += $wrote) {
      $wrote = @fwrite($sock, substr($buf, $total));
      if ($wrote === false || $wrote < 1) {
        $err = error_get_last();
        if (is_array($err)) {
          $reason = idx($err, 'message', false);
        }  else {
          $reason = '';
        }
        return sprintf("wrote %d/%d. %s", $total, strlen($buf), $reason);
      }
    }
    return true;
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
    $buf = $req . "\n";
    $why = $this->fwrite_all($this->sock, $buf);
    if ($why !== true) {
      echo "Failed to send $buf$why\n";
      throw new Exception($why);
    }
    return $this->readResponses();
  }

  private function renderVGStack($stack) {
    $text = '';
    foreach ($stack->frame as $frame) {
      if ($frame->file) {
        $origin = sprintf("%s:%d", $frame->file, $frame->line);
      } else {
        $origin = (string)$frame->obj;
      }
      $text .= sprintf("\n  %40s   %s",
        $origin,
        $frame->fn);
    }
    return $text;
  }

  private function renderVGResult($err) {
    $text = "";
    $want = array(
      'stack' => true,
      'xwhat' => true,
      'what' => true,
      'auxwhat' => true,
    );
    foreach ($err as $k => $elem) {
      if (!isset($want[$k])) {
        continue;
      }
      if ($k == 'stack') {
        $v = $this->renderVGStack($elem);
      } elseif ($k == 'xwhat') {
        $v = (string)$elem->text;
      } elseif ($k == 'what') {
        $v = (string)$elem;
      } elseif ($k == 'auxwhat') {
        $v = (string)$elem;
      }

      if (strlen($text)) $text .= "\n";
      $text .= $v;
    }

    return $text;
  }

  private function waitForTerminate($timeout) {
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

  protected function waitForSuspendedState($suspended, $timeout, $pid = null) {
    if (phutil_is_windows()) {
      return true;
    }
    if ($pid === null) {
      $st = proc_get_status($this->proc);
      $pid = $st['pid'];
    }
    // The response to proc_get_status has a 'stopped' value, which is
    // ostensibly set to a truthy value if the process is stopped and falsy if
    // it isn't. Why don't we use it, you ask? Well, let me ask you a question
    // in response. What do you expect the following code to print out?
    //
    // $st = proc_get_status($this->proc);
    // posix_kill($st['pid'], SIGSTOP);
    // -- wait for a while so that the process is stopped --
    // $st = proc_get_status($this->proc);
    // print ((bool)$st['stopped']).', ';
    // $st = proc_get_status($this->proc);
    // print (bool)$st['stopped'];
    //
    // If you said 'true, true', congratulations! You're a reasonable
    // person. However, PHP is well known to not be reasonable, and in reality
    // 'true, false' will be printed out. That is because proc_get_status only
    // returns a truthy value for 'stopped' the first time it is called after
    // the process is stopped. Subsequent calls return a falsy value for it.
    //
    // To solve this, we resort to good old ps. This will hopefully be portable
    // enough.
    $deadline = time() + $timeout;
    $state = (PHP_OS == 'SunOS') ? 's' : 'state';
    do {
      list($stdout, $_) = execx('ps -o %s -p %s | tail -n 1', $state, $pid);
      $stdout = trim($stdout);
      if ($stdout === '') {
        throw new Exception('ps returned nothing -- did watchman go away?');
      }
      // Linux returns 'T', but OS X can return 'T+' etc.
      $is_stopped = (bool)preg_match('/T/', $stdout);
      if ($suspended === $is_stopped) {
        return true;
      }
      usleep(30000);
    } while (time() <= $deadline);
    return false;
  }

  private function appendLogFile($label, $srcname, $destname) {
    $fp = fopen($srcname, 'r');
    $dest = fopen($destname, 'a');
    fprintf($dest, "\n=== $label from $srcname ===\n");
    stream_copy_to_stream($fp, $dest);
    fprintf($dest, "\n");
    fclose($fp);
    fclose($dest);
  }

  function terminateProcess() {
    if (!$this->proc) {
      return;
    }
    $this->resumeProcess();
    $timeout = $this->valgrind ? 20 : 5;
    if ($this->sock) {
      try {
        $this->request('shutdown-server');
      } catch (Exception $e) {
        printf("shutdown-server: %s\n", $e->getMessage());
      }
      $st = $this->waitForTerminate($timeout);
    } else {
      $st = proc_get_status($this->proc);
    }

    if ($st['running']) {
      echo "Didn't stop after $timeout seconds, sending signal\n";
      system("gstack " . $st['pid']);
      proc_terminate($this->proc);
      $st = $this->waitForTerminate($timeout);
      if ($st['running']) {
        echo "Still didn't stop, sending bigger signal\n";
        proc_terminate($this->proc, 9);
        $st = $this->waitForTerminate(5);
      }
    }
    if ($st['running']) {
      echo "Why won't you die!???\n";
      var_dump($st);
    }
    $this->proc = null;
    if ($this->debug) {
      readfile($this->logfile.'.log');
    }
    $TMP = phutil_is_windows() ? '' : '/tmp/';
    $this->appendLogFile(
      'config',
      $this->config_file,
      $TMP . 'watchman-test.log'
    );
    $this->appendLogFile(
      'output',
      $this->logfile,
      $TMP . 'watchman-test.log'
    );
    if (file_exists($this->vg_log.'.xml')) {
      $this->appendLogFile(
        'valgrind',
        $this->vg_log.'.xml',
        $TMP . "watchman-valgrind.xml"
      );
    }
    if (file_exists($this->vg_log)) {
      $this->appendLogFile(
        'valgrind',
        $this->vg_log,
        $TMP . 'watchman-valgrind.log'
      );
    }
    if (file_exists($this->cg_file)) {
      copy($this->cg_file, $TMP . "/watchman-callgrind.txt");
    }
  }

  public function suspendProcess() {
    if (!$this->proc) {
      throw new Exception("watchman process isn't running");
    }
    $timeout = $this->valgrind ? 20 : 5;
    $st = proc_get_status($this->proc);
    if (!$st['running']) {
      throw new Exception('watchman process terminated');
    }
    if (phutil_is_windows()) {
      execx('susres.exe suspend %d', $st['pid']);
    } else {
      // SIGSTOP isn't defined on the default PHP shipped with OS X, so use kill
      execx('kill -STOP %s', $st['pid']);
    }
    if (!$this->waitForSuspendedState(true, $timeout)) {
      throw new Exception("watchman process didn't stop in $timeout seconds");
    }
  }

  public function resumeProcess() {
    if (!$this->proc) {
      throw new Exception("watchman process isn't running");
    }
    $timeout = $this->valgrind ? 20 : 5;
    $st = proc_get_status($this->proc);
    if (!$st['running']) {
      throw new Exception('watchman process terminated');
    }
    if (phutil_is_windows()) {
      execx('susres.exe resume %d', $st['pid']);
    } else {
      // SIGCONT isn't defined on the default PHP shipped with OS X, so use kill
      execx('kill -CONT %s', $st['pid']);
    }
    if (!$this->waitForSuspendedState(false, $timeout)) {
      throw new Exception("watchman process didn't resume in $timeout seconds");
    }
  }

  function __destruct() {
    $this->terminateProcess();
  }

}

function execx() {
  $args = func_get_args();
  printf("# execx: %s\n", json_encode($args));
  if (count($args) > 1) {
    $cmd = call_user_func_array('sprintf', $args);
  } else {
    $cmd = $args[0];
  }
  exec($cmd, $output, $status);
  if ($status != 0) {
    throw new Exception("$cmd failed with status $status $output");
  }
  return array(implode("\n", $output), null);
}

// This is a helper to avoid having to spawn a new watchman
// process for every php test that we launch via the python
// harness
class PythonProvidedWatchmanInstance extends WatchmanInstance {
  public function suspendProcess() {
    $timeout = 5;
    // SIGSTOP isn't defined on the default PHP shipped with OS X, so use kill
    execx('kill -STOP %s', $this->pid);
    if (!$this->waitForSuspendedState(true, $timeout, $this->pid)) {
      throw new Exception("watchman process didn't stop in $timeout seconds");
    }
  }

  public function resumeProcess() {
    $timeout = 5;
    // SIGCONT isn't defined on the default PHP shipped with OS X, so use kill
    execx('kill -CONT %s', $this->pid);
    if (!$this->waitForSuspendedState(false, $timeout, $this->pid)) {
      throw new Exception("watchman process didn't resume in $timeout seconds");
    }
  }

  function waitForLogOutput($criteria, $timeout = 5) {
    // We can't find the log file for the python test instance from here,
    // so the test should run its own instance
    throw new Exception('you must return a non-empty array from getGlobalConfig');
  }

  function __construct() {
    $this->sockname = getenv('WATCHMAN_SOCK');
  }

  function getFullSockName() {
    return $this->sockname;
  }

  function start() {
    $this->openSock();
    $this->pid = $this->getProcessID();
  }

  function terminateProcess() {
  }
}


// vim:ts=2:sw=2:et:
