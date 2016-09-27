<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function getGlobalConfig() {
    return array(
      // We need to run our own instance so that we can look in
      // the log file
      'dummy' => true
    );
  }

  function doesTriggerDataMatchFileList($root, array $files) {
    $trigger_json = $root . DIRECTORY_SEPARATOR . 'trigger.json';

    if (!file_exists($trigger_json)) {
      return array(false, 'no such file');
    }

    // Validate that the json input is properly formatted
    $expect = array();
    foreach ($files as $file) {
      $expect[] = array(
        'name' => $file,
        'exists' => true
      );
    }
    usort($expect, function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });

    $lines = 0;
    $got = array();
    foreach (@file($trigger_json) as $line) {
      $lines++;
      $list = json_decode($line, true);
      // Filter out the unpredictable data from lstat()
      $list = array_map(function ($ent) {
          return array(
            'name' => $ent['name'],
            'exists' => $ent['exists']
          );
        }, $list);

      usort($list, function ($a, $b) {
        return strcmp($a['name'], $b['name']);
      });

      foreach ($list as $ele) {
        $got[] = $ele;
      }
    }
    $got = array_unique($got, SORT_REGULAR);

    if ($expect === $got) {
      return array(true, 'matches');
    }

    return array(false, "expect: ".json_encode($expect) .
      " got: " . json_encode($got));
  }

  function validateTriggerOutput($root, array $files, $context) {
    $trigger_log = $root . DIRECTORY_SEPARATOR . "trigger.log";
    $trigger_json = $root . DIRECTORY_SEPARATOR . "trigger.json";

    $this->waitFor(function () use ($root, $files, $trigger_log) {
      if (file_exists($trigger_log)) {
        $dat = file_get_contents($trigger_log);
        $n = 0;
        foreach ($files as $file) {
          if (strpos($dat, $file) !== false) {
            $n++;
          }
        }
        return $n == count($files);
      }
      return false;
    }, 5, function () use ($root, $files, $context, $trigger_log) {
      return sprintf(
        "[$context] $trigger_log should contain %s, has %s",
        json_encode($files),
        file_get_contents($trigger_log)
      );
    });

    $logdata = file_get_contents($trigger_log);
    foreach ($files as $file) {
      $this->assertRegex(
        "/$file/m",
        $logdata,
        "[$context] got the right filename in $trigger_log"
      );
    }

    $self = $this;
    $this->waitFor(function () use ($root, $files, $self) {
      list ($ok, $why) = $self->doesTriggerDataMatchFileList($root, $files);
      return $ok;
    }, 5, function () use ($root, $files, $context, $self, $trigger_json) {
      list ($ok, $why) = $self->doesTriggerDataMatchFileList($root, $files);
      return sprintf(
        "[$context] trigger.json holds valid json for %s: %s",
        json_encode($files),
        $why
      );
    });
  }

  function testLegacyTrigger() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    touch("$root/foo.c");
    touch("$root/b ar.c");
    touch("$root/bar.txt");

    file_put_contents("$root/.watchmanconfig", json_encode(
      array('settle' => 200)
    ));

    $out = $this->watch($root);

    $this->assertFileList($root, array(
      '.watchmanconfig', 'b ar.c', 'bar.txt', 'foo.c'
    ));

    $res = $this->trigger($root,
      'test', '*.c', '--',
      $_ENV['WATCHMAN_PYTHON_BINARY'],
      dirname(__FILE__) . DIRECTORY_SEPARATOR . 'trig.py',
      $root . DIRECTORY_SEPARATOR . "trigger.log");
    $this->assertEqual('created', idx($res, 'disposition'));

    $this->trigger($root,
      'other', '*.c', '--',
      $_ENV['WATCHMAN_PYTHON_BINARY'],
      dirname(__FILE__) . DIRECTORY_SEPARATOR . 'trigjson.py',
      $root . DIRECTORY_SEPARATOR . "trigger.json");

    $trig_list = array(
      array(
        'append_files' => true,
        'name' => 'other',
        'command' => array(
          $_ENV['WATCHMAN_PYTHON_BINARY'],
          dirname(__FILE__) . DIRECTORY_SEPARATOR . 'trigjson.py',
          $root . DIRECTORY_SEPARATOR . "trigger.json"
        ),
        'expression' => array(
          'anyof',
          array('match', '*.c', 'wholename')
        ),
        'stdin' => array('name', 'exists', 'new', 'size', 'mode'),
      ),
      array(
        'append_files' => true,
        'name' => 'test',
        'command' => array(
          $_ENV['WATCHMAN_PYTHON_BINARY'],
          dirname(__FILE__) . DIRECTORY_SEPARATOR . 'trig.py',
          $root . DIRECTORY_SEPARATOR . "trigger.log"
        ),
        'expression' => array(
          'anyof',
          array('match', '*.c', 'wholename')
        ),
        'stdin' => array('name', 'exists', 'new', 'size', 'mode'),
      ),
    );

    $this->assertTriggerList($root, $trig_list);


    $this->startLogging('debug');

    $this->suspendWatchman();
    touch("$root/foo.c");
    touch("$root/b ar.c");
    $this->resumeWatchman();

    $this->watchmanCommand('log', 'debug', 'waiting for spawnp ' . __LINE__);
    $this->assertWaitForLog('/posix_spawnp: test/');
    $this->assertWaitForLog('/posix_spawnp: other/');
    $this->assertWaitForLogOutput('/WOOT from trig/');

    $this->stopLogging();

    $this->validateTriggerOutput($root, array('foo.c', 'b ar.c'), 'initial');

    foreach (array('foo.c', 'b ar.c') as $file) {
      // Validate that we observe the updates correctly
      // (that we're handling the since portion of the query)
      $this->suspendWatchman();
      w_unlink("$root/trigger.log");
      w_unlink("$root/trigger.json");
      touch("$root/$file");
      $this->resumeWatchman();
      $this->validateTriggerOutput($root, array($file), "single $file");
    }

    w_unlink("$root/trigger.log");
    w_unlink("$root/trigger.json");

    // When running under valgrind, there may be pending events.
    // Let's give things a chance to finish dispatching before proceeding
    $this->waitForNoThrow(function () use ($root) {
      return file_exists("$root/trigger.log");
    }, 1);

    @w_unlink("$root/trigger.log");
    @w_unlink("$root/trigger.json");

    $this->startLogging('debug');

    // trigger a recrawl
    $this->watchmanCommand('debug-recrawl', $root);

    // make sure the triggers didn't get deleted
    $this->assertTriggerList($root, $trig_list);

    $this->watchmanCommand('log', 'debug', 'waiting for spawnp ' . __LINE__);
    $this->assertWaitForLog('/posix_spawnp/', 5);
    $this->stopLogging();

    // and that the right data was seen
    $this->validateTriggerOutput($root,
      array('foo.c', 'b ar.c'), 'after recrawl');

    $res = $this->trigger($root, 'other', '*.c', '--', 'true');
    $this->assertEqual('replaced', idx($res, 'disposition'));

    $res = $this->trigger($root, 'other', '*.c', '--', 'true');
    $this->assertEqual('already_defined', idx($res, 'disposition'));

    $res = $this->watchmanCommand('trigger-del', $root, 'test');
    $this->assertEqual(true, $res['deleted']);
    $this->assertEqual('test', $res['trigger']);

    $triggers = $this->watchmanCommand('trigger-list', $root);
    $this->assertEqual(1, count($triggers['triggers']));

    $res = $this->watchmanCommand('trigger-del', $root, 'other');
    $this->assertEqual(true, $res['deleted']);
    $this->assertEqual('other', $res['trigger']);

    $triggers = $this->watchmanCommand('trigger-list', $root);
    $this->assertEqual(0, count($triggers['triggers']));
  }

}

// vim:ts=2:sw=2:et:
