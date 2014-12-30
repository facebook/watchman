<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class infoTestCase extends WatchmanTestCase {

  function testSockName() {
    $resp = $this->watchmanCommand('get-sockname');
    $this->assertEqual($resp['sockname'],
                       $this->watchman_instance->getFullSockName());
  }

  function testGetConfigEmpty() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $this->watch($root);
    $res = $this->watchmanCommand('get-config', $root);
    $this->assertEqual(array(), $res['config']);
  }

  function testGetConfig() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $cfg = array(
      'test-key' => 'test-value',
    );
    file_put_contents("$root/.watchmanconfig", json_encode($cfg));

    $this->watch($root);
    $res = $this->watchmanCommand('get-config', $root);
    $this->assertEqual($cfg, $res['config']);
  }
}
