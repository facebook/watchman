<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class modifyTest extends WatchmanTestCase {
  function needsLiveConnection() {
    return true;
  }

  function testModify() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();

    mkdir("$root/foo");
    touch("$root/foo/111");
    $watch = $this->watch($root);
    $this->watchmanCommand('subscribe', $root, 'test',
      array(
        'fields' => array('name'),
        'expression' => array('type', 'f'),
      )
    );
    $this->waitForSub('test', function($data) { return true; });
    $this->getSubData('test');

    $this->watchmanCommand('log', 'error', 'XXX: touch foo/111');
    touch("$root/foo/111");

    $this->waitForSub('test', function($data) { return true; });
    list($sub) = $this->getSubData('test');
    $this->assertEqualFileList(array("foo/111"), $sub['files']);

    $this->watchmanCommand('unsubscribe', $root, 'test');
  }
}
