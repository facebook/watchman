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

class basicTestCase extends WatchmanTestCase {

  function testFind() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $out = $this->watchmanCommand('watch', $root);
    $this->assertEqual($root, $out['watch']);

    // Allow time for the files to be found
    for ($tries = 0; $tries < 20; $tries++) {
      $out = $this->watchmanCommand('find', $root, '*.c');
      if (count($out['files'])) {
        break;
      }
      usleep(2000);
    }

    $this->assertEqual('foo.c', $out['files'][0]['name']);
    $this->assertEqual(1, count($out['files']), 'only one match');
  }

  function testCursor() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);

    $initial = $this->watchmanCommand('since', $root,
      'n:testCursor');

    $this->assertRegex('/^c:\d+:\d+$/', $initial['clock'],
      "clock seemslegit");

    touch($root . '/one');

    // Allow time for the change to be observed
    for ($tries = 0; $tries < 20; $tries++) {
      $update = $this->watchmanCommand('since', $root,
        'n:testCursor');
      if (count($update['files'])) {
        break;
      }
      usleep(30000);
    }
    $this->assertEqual('one',
      $update['files'][0]['name'], 'saw file change');

    $later = $this->watchmanCommand('since', $root,
      'n:testCursor');
    $this->assertEqual(array(), $later['files'], 'no changes');
  }
}

// vim:ts=2:sw=2:et:

