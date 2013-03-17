<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class cursorTestCase extends WatchmanTestCase {
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
    $this->assertEqual(true,
      $update['files'][0]['new'], 'shows as new');

    $later = $this->watchmanCommand('since', $root,
      'n:testCursor');
    $this->assertEqual(array(), $later['files'], 'no changes');

    /* now to verify that the file doesn't show as new after this next
     * change */
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
    $this->assertEqual(false,
      isset($update['files'][0]['new']), 'not new');
  }
}
