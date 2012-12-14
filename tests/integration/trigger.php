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
      '*.c', '--', dirname(__FILE__) . '/trig.sh', "$root/trigger.log");
    $this->assertRegex('/^\d+$/', $out['triggerid']);

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
  }

}

// vim:ts=2:sw=2:et:

