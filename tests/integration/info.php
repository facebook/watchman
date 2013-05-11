<?php

/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class infoTestCase extends WatchmanTestCase {

  function testSockName() {
    $resp = $this->watchmanCommand('get-sockname');
    $this->assertEqual($resp['sockname'],
                       WatchmanInstance::get()->getFullSockName());
  }
}
