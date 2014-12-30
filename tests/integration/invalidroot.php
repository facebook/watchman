<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class InvalidRootTestCase extends WatchmanTestCase {
  function testInvalidRoot() {
    $dir = new WatchmanDirectoryFixture();
    $invalid = $dir->getPath('invalid');

    $res = $this->watch("$invalid", false);
    $this->assertEqual(
      "unable to resolve root $invalid: ".
      "realpath($invalid) -> No such file or directory",
      $res['error']
    );
  }

}
