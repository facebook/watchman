<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class cursorTestCase extends WatchmanTestCase {
  function testCursor() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);

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
    $this->assertEqual(false, isset($since['files'][0]['new']));
  }
}
