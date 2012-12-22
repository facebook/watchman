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

class triggerTestCase extends WatchmanTestCase {

  function testTrigger() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $out = $this->watchmanCommand('watch', $root);
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

    $this->setLogLevel('debug');

    touch("$root/foo.c");

    $this->assertWaitForLog('/posix_spawnp/', 60);

    $this->setLogLevel('off');

    $this->waitFor(function () use ($root) {
      return file_exists("$root/trigger.log");
    }, 5, "created trigger.log");

    $this->assertRegex('/^foo.c$/m',
      file_get_contents("$root/trigger.log"),
      "got the right filename in the log");

    // Validate that the json input is properly formatted
    $lines = 0;
    foreach (file("$root/trigger.json") as $line) {
      $lines++;
      $list = json_decode($line, true);
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
    $triggers = $this->watchmanCommand('trigger-list', $root);
    $triggers = $triggers['triggers'];
    usort($triggers, function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual($trig_list, $triggers);

    $out = $this->watchmanCommand('trigger', $root,
                  'other', '*.c', '--', 'true');
    $this->assertEqual('other', $out['triggerid']);
  }

}

// vim:ts=2:sw=2:et:

