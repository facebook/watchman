<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerTestCase extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  // Assumes that there is exactly one file in the trigger output: foo.c.
  function validateTriggerOutput($root) {
    $this->waitFor(function () use ($root) {
      if (file_exists("$root/trigger.log")) {
        return preg_match(
          '/foo.c/',
          file_get_contents("$root/trigger.log")
        );
      }
      return false;
    }, 5, "created trigger.log");

    $logdata = file_get_contents("$root/trigger.log");
    $this->assertRegex('/^foo.c$/m',
      $logdata,
      "got the right filename in the log");

    // Validate that the json input is properly formatted
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
      $this->assertEqual(array(
        array(
          'name' => 'foo.c',
          'exists' => true
        )
      ), $list);
    }
    if ($lines == 0) {
      $this->assertFailure("No json lines seen");
    }
  }

  function assertTriggerList($root, $trig_list) {
    $triggers = $this->watchmanCommand('trigger-list', $root);
    $triggers = $triggers['triggers'];
    usort($triggers, function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual($trig_list, $triggers);
  }

  function testTrigger() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $out = $this->watch($root);
    $this->assertEqual($root, $out['watch']);

    $this->assertFileList($root, array('bar.txt', 'foo.c'));

    $out = $this->watchmanCommand('trigger', $root,
      'test', '*.c', '--', dirname(__FILE__) . '/trig.sh',
      "$root/trigger.log");
    $this->assertEqual('test', $out['triggerid']);

    $out = $this->watchmanCommand('trigger', $root,
      'other', '*.c', '--', dirname(__FILE__) . '/trigjson',
      "$root/trigger.json");
    $this->assertEqual('other', $out['triggerid']);

    $this->startLogging('debug');

    touch("$root/foo.c");

    $this->assertWaitForLog('/posix_spawnp/', 60);

    $this->stopLogging();

    $this->validateTriggerOutput($root);

    $trig_list = array(
      array(
        'name' => 'other',
        'rules' => array(
          array(
            'pattern' => '*.c',
            'include' => true,
            'negated' => false
          ),
        ),
        'command' => array(
          dirname(__FILE__) . '/trigjson',
          "$root/trigger.json"
        ),
      ),
      array(
        'name' => 'test',
        'rules' => array(
          array(
            'pattern' => '*.c',
            'include' => true,
            'negated' => false
          ),
        ),
        'command' => array(
          dirname(__FILE__) . '/trig.sh',
          "$root/trigger.log"
        ),
      ),
    );

    $this->assertTriggerList($root, $trig_list);

    // trigger a recrawl
    unlink("$root/trigger.log");
    unlink("$root/trigger.json");
    $this->startLogging('debug');
    $this->watchmanCommand('debug-recrawl', $root);

    // make sure the triggers didn't get deleted
    $this->assertTriggerList($root, $trig_list);

    $this->assertWaitForLog('/posix_spawnp/', 60);
    $this->stopLogging();

    // and that the right data was seen
    $this->validateTriggerOutput($root);

    $out = $this->watchmanCommand('trigger', $root,
                  'other', '*.c', '--', 'true');
    $this->assertEqual('other', $out['triggerid']);

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

