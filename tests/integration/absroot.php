<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class absoluteRootTestCase extends WatchmanTestCase {
  function testDot() {
    $res = $this->watch('.', false);
    $this->assertEqual(
      'unable to resolve root .: path "." must be absolute',
      idx($res, 'error')
    );
  }

  function testSlash() {
    $res = $this->watch('/', false);
    $this->assertEqual(
      'unable to resolve root /: cannot watch "/"',
      idx($res, 'error')
    );
  }
}
