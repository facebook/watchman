<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function validateTriggerOutput($root, array $files) {
    $this->waitFor(function () use ($root, $files) {
      if (file_exists("$root/trigger.log")) {
        $dat = file_get_contents("$root/trigger.log");
        $n = 0;
        foreach ($files as $file) {
          if (strpos($dat, $file) !== false) {
            $n++;
          }
        }
        return $n == count($files);
      }
      return false;
    }, 5, function () use ($root, $files) {
      return sprintf(
        "trigger.log should contain %s, has %s",
        json_encode($files),
        file_get_contents("$root/trigger.log")
      );
    });

    $logdata = file_get_contents("$root/trigger.log");
    foreach ($files as $file) {
      $this->assertRegex(
        "/$file/m",
        $logdata,
        "got the right filename in the log"
      );
    }

    $this->waitFor(function () use ($root) {
      if (file_exists("$root/trigger.json")) {
        return json_decode(
          file_get_contents("$root/trigger.json")
        );
      }
      return false;
    }, 5, "created trigger.json");

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
    foreach (file("$root/trigger.json") as $line) {
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
      $this->assertEqual($expect, $list);
    }
    if ($lines == 0) {
      $this->assertFailure("No json lines seen");
    }
  }

  function testLegacyTrigger() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/b ar.c");
    touch("$root/bar.txt");

    $out = $this->watch($root);
    $this->assertEqual($root, $out['watch']);

    $this->assertFileList($root, array('b ar.c', 'bar.txt', 'foo.c'));

    $out = $this->trigger($root,
      'test', '*.c', '--', dirname(__FILE__) . '/trig.sh',
      "$root/trigger.log");

    $out = $this->trigger($root,
      'other', '*.c', '--', dirname(__FILE__) . '/trigjson',
      "$root/trigger.json");

    $trig_list = array(
      array(
        'append_files' => true,
        'name' => 'other',
        'command' => array(
          dirname(__FILE__) . '/trigjson',
          "$root/trigger.json"
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
          dirname(__FILE__) . '/trig.sh',
          "$root/trigger.log"
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
    $this->assertWaitForLog('/posix_spawnp/');
    $this->assertWaitForLogOutput('/WOOT from trig.sh/');

    $this->stopLogging();

    $this->validateTriggerOutput($root, array('foo.c', 'b ar.c'));

    foreach (array('foo.c', 'b ar.c') as $file) {
      // Validate that we observe the updates correctly
      // (that we're handling the since portion of the query)
      unlink("$root/trigger.log");
      unlink("$root/trigger.json");
      touch("$root/$file");
      $this->validateTriggerOutput($root, array($file));
    }

    // trigger a recrawl
    unlink("$root/trigger.log");
    unlink("$root/trigger.json");
    $this->startLogging('debug');
    $this->watchmanCommand('debug-recrawl', $root);

    // make sure the triggers didn't get deleted
    $this->assertTriggerList($root, $trig_list);

    $this->watchmanCommand('log', 'debug', 'waiting for spawnp ' . __LINE__);
    $this->assertWaitForLog('/posix_spawnp/', 5);
    $this->stopLogging();

    // and that the right data was seen
    $this->validateTriggerOutput($root, array('foo.c', 'b ar.c'));

    $out = $this->trigger($root,
                  'other', '*.c', '--', 'true');

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
