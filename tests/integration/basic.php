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
    $out = $this->waitForWatchman(
      array('find', $root),
      function ($out) {
        return count($out['files']) == 2;
      }
    );

    usort($out['files'], function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual(array(
      array(
        'name' => 'bar.txt',
        'exists' => true
      ),
      array(
        'name' => 'foo.c',
        'exists' => true
      ),
    ), $out['files']);

    // Make sure we correctly observe deletions
    unlink("$root/bar.txt");

    $out = $this->waitForWatchman(
      array('find', $root),
      function ($out) {
        foreach ($out['files'] as $ent) {
          if ($ent['name'] == 'bar.txt' &&
              $ent['exists'] === false) {
            return true;
          }
        }
        return false;
      }
    );

    usort($out['files'], function ($a, $b) {
      return strcmp($a['name'], $b['name']);
    });
    $this->assertEqual(array(
      array(
        'name' => 'bar.txt',
        'exists' => false
      ),
      array(
        'name' => 'foo.c',
        'exists' => true
      ),
    ), $out['files']);

    // A moderately more complex set of changes
    mkdir("$root/adir");
    mkdir("$root/adir/subdir");
    touch("$root/adir/subdir/file");
    rename("$root/adir/subdir", "$root/adir/overhere");

    $this->watchmanCommand('log', "renamed adir/subdir to adir/overhere");

    // Harder to do a direct comparison

    $expect = array(
      'adir/overhere/file',
      'foo.c',
    );
    $out = $this->waitForWatchman(
      array('find', $root),
      function ($out) use ($this, $expect) {
        return $this->sortedListOfExistingFiles($out['files']) == $expect;
      }
    );

    $this->assertEqual(
      $this->sortedListOfExistingFiles($out['files']),
      $expect);

    $this->watchmanCommand('log', 'renamed adir to bdir');
    rename("$root/adir", "$root/bdir");

    $expect = array(
      'bdir/overhere/file',
      'foo.c',
    );
    $out = $this->waitForWatchman(
      array('find', $root),
      function ($out) use ($this, $expect) {
        return $this->sortedListOfExistingFiles($out['files']) == $expect;
      }
    );

    $this->assertEqual(
      $this->sortedListOfExistingFiles($out['files']),
      $expect);
  }

  function sortedListOfExistingFiles($file_list) {
    $files = array();
    foreach ($file_list as $ent) {
      if ($ent['exists']) {
        $files[] = $ent['name'];
      }
    }
    sort($files);
    return $files;
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
    $update = $this->waitForWatchman(
      array('since', $root, 'n:testCursor'),
      function ($update) {
        return count($update['files']);
      }
    );

    $this->assertEqual('one',
      $update['files'][0]['name'], 'saw file change');

    $later = $this->watchmanCommand('since', $root,
      'n:testCursor');
    $this->assertEqual(array(), $later['files'], 'no changes');
  }
}

// vim:ts=2:sw=2:et:

