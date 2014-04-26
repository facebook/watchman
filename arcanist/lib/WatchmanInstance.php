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
  private $config_file;
  private $debug = false;
  private $valgrind = false;
  private $coverage = false;
  private $vg_log;
  private $cg_file;
  private $sock;
  private $repo_root;
  private $logdata = array();
  private $subdata = array();
  const TIMEOUT = 20;

  function __construct($repo_root, $coverage, $config = array()) {
    $this->repo_root = $repo_root;
    $this->logfile = new TempFile();
    $this->sockname = new TempFile();
    $this->config_file = new TempFile();
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
      $this->vg_log = new TempFile();
    } elseif ($coverage) {
      $this->coverage = true;
      $this->cg_file = new TempFile();
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

  function startLogging($level) {
    $this->logdata = array();
    return $this->request('log-level', $level);
  }

  function stopLogging() {
    return $this->request('log-level', 'off');
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
    $resp = $this->request('get-pid');
    return $resp['pid'];
  }

  // Looks in the log file for the matching term.
  // This is useful for checking stdout/stderr from triggers
  function waitForLogOutput($criteria, $timeout = 5) {
    $deadline = time() + $timeout;
    while (time() < $deadline) {
      foreach (file($this->logfile) as $line) {
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
    return $this->sockname . '.sock';
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
    } else if ($this->coverage) {
      $cmd = "valgrind --tool=callgrind --collect-jumps=yes " .
        "--separate-recs=16 --callgrind-out-file=$this->cg_file " .
        $cmd;
    }
    $cmd = "WATCHMAN_CONFIG_FILE=%s " . $cmd;

    $cmd = csprintf($cmd, $this->config_file, $this->sockname, $this->logfile,
                    $this->logfile);

    $pipes = array();
    $this->proc = proc_open($cmd, array(
      0 => array('file', '/dev/null', 'r'),
      1 => array('file', $this->logfile, 'a'),
      2 => array('file', $this->logfile, 'a'),
    ), $pipes);

    if (!$this->proc) {
      throw new Exception("Failed to spawn $cmd");
    }

    $sockname = $this->getFullSockName();
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

  function generateCoverageResults() {
    // Find all executables, not just the test executables.
    // We need to do this because the test executables may not
    // include all of the possible code (linker decides to omit
    // it from the image) so we see a skewed representation of
    // the source lines.

    $fp = popen(
      "find $this->repo_root -type f -name watchman -o " .
      "-name \*.a -o -name \*.so -o -name \*.dylib", "r");
    while (true) {
      $line = fgets($fp);
      if ($line === false) break;
      $obj_files[] = trim($line);
    }

    // Parse line information from the objects
    foreach ($obj_files as $object) {
      DwarfLineInfo::loadObject($object);
    }

    $CG = new CallgrindFile((string)$this->cg_file);
    $CG->parse();

    $source_files = array();
    foreach ($CG->getSourceFiles() as $filename) {
      if (Filesystem::isDescendant($filename, $this->repo_root) &&
        !preg_match("/(thirdparty|tests)/", $filename)) {
          $source_files[$filename] = $filename;
        }
    }

    $cov = array();
    foreach ($source_files as $filename) {
      $relsrc = substr($filename, strlen($this->repo_root) + 1);
      $cov[$relsrc] = CallgrindFile::mergeSourceLineData(
        $filename, array($CG));
    }

    $res = new ArcanistUnitTestResult();
    $res->setName('coverage');
    $res->setUserData("Collected");
    $res->setResult(ArcanistUnitTestResult::RESULT_PASS);
    $res->setCoverage($cov);

    // Stash it for review with our `arc cov` command
    $wc = ArcanistWorkingCopyIdentity::newFromPath($this->repo_root);
    $api = ArcanistRepositoryAPI::newAPIFromWorkingCopyIdentity($wc);
    $api->writeScratchFile('wman-cov.json', json_encode($cov));

    return array($res);
  }

  function generateValgrindTestResults() {
    $this->terminateProcess();

    if ($this->coverage) {
      return $this->generateCoverageResults();
    }

    if (!$this->valgrind) {
      return array();
    }

    $definite_leaks = array();
    $possible_leaks = array();
    $errors = array();
    $descriptors = array();

    // valgrind seems to use an interesting definition of valid XML.
    // Tolerate having multiple documents in one file.
    // Confluence of weird bugs; hhvm has very low preg_match limits
    // so we have to grovel around to make sure that we read this
    // stuff in properly :-/
    $documents = array();
    $in_doc = false;
    $doc = null;
    foreach (file($this->vg_log . '.xml') as $line) {
      if ($in_doc) {
        $doc[] = $line;
        if (preg_match(',</valgrindoutput>,', $line)) {
          $documents[] = implode("\n", $doc);
          $doc = null;
        }
      } else {
        if (preg_match(',<valgrindoutput>,', $line)) {
          $doc = array($line);
          $in_doc = true;
        }
      }
    }
    foreach ($documents as $data) {
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
        var_dump($vg);
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

  private function waitForSuspendedState($suspended, $timeout) {
    $st = proc_get_status($this->proc);
    $pid = $st['pid'];
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
    do {
      list($stdout, $_) = execx('ps -o state -p %s | tail -n 1', $pid);
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

  function terminateProcess() {
    if (!$this->proc) {
      return;
    }
    $timeout = $this->valgrind ? 20 : 5;
    if ($this->sock) {
      $this->request('shutdown-server');
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
      readfile($this->logfile);
    }
    copy($this->logfile, '/tmp/watchman-test.log');
    if (file_exists($this->vg_log.'.xml')) {
      copy($this->vg_log.'.xml', "/tmp/watchman-valgrind.xml");
    }
    if (file_exists($this->vg_log)) {
      copy($this->vg_log, "/tmp/watchman-valgrind.txt");
    }
    if (file_exists($this->cg_file)) {
      copy($this->cg_file, "/tmp/watchman-callgrind.txt");
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
    // SIGSTOP isn't defined on the default PHP shipped with OS X, so use kill
    execx('kill -STOP %s', $st['pid']);
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
    // SIGCONT isn't defined on the default PHP shipped with OS X, so use kill
    execx('kill -CONT %s', $st['pid']);
    if (!$this->waitForSuspendedState(false, $timeout)) {
      throw new Exception("watchman process didn't resume in $timeout seconds");
    }
  }

  function __destruct() {
    $this->terminateProcess();
  }

}

// vim:ts=2:sw=2:et:
