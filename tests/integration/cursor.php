<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class cursorTestCase extends WatchmanTestCase {
  function testCursor() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $watch = $this->watch($root);

    $this->watchmanCommand('log', 'debug', 'XXX 1st since testCursor');
    $this->assertFileListUsingSince($root, 'n:testCursor', array());

    $this->watchmanCommand('log', 'debug', 'XXX touch one');
    touch($root . '/one');

    $this->watchmanCommand('log', 'debug', 'XXX 2nd since testCursor');
    $since = $this->assertFileListUsingSince(
                $root, 'n:testCursor', array('one'));
    $this->assertEqual(true, $since['files'][0]['new']);

    $this->watchmanCommand('log', 'debug', 'XXX 3rd since testCursor');
    $this->assertFileListUsingSince(
                $root, 'n:testCursor', array('one'), array());

    $this->watchmanCommand('log', 'debug', 'XXX 2nd touch one');
    /* now to verify that the file doesn't show as new after this next
     * change */
    touch($root . '/one', time()+1);
    $since = $this->assertFileListUsingSince(
                  $root, 'n:testCursor', array('one'));
    $this->assertEqual(false, $since['files'][0]['new']);

    // Deleted files shouldn't show up in fresh cursors
    touch("$root/two");
    unlink("$root/one");
    $res = $this->watchmanCommand('since', $root, 'n:testCursor2');
    $this->assertEqual(true, $res['is_fresh_instance']);
    $this->assertEqual(1, count($res['files']));
    $this->assertEqual('two', $res['files'][0]['name']);
    $this->assertEqual(true, $res['files'][0]['exists']);

    // ... but they should show up afterwards
    unlink("$root/two");
    $res = $this->watchmanCommand('since', $root, 'n:testCursor2');
    $this->assertEqual(false, $res['is_fresh_instance']);
    $this->assertEqual(1, count($res['files']));
    $this->assertEqual('two', $res['files'][0]['name']);
    $this->assertEqual(false, $res['files'][0]['exists']);
  }
}
