<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class absoluteRootTestCase extends WatchmanTestCase {
  function testDot() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $here = getcwd();
    chdir($root);

    try {
      $is_cli = $this->isUsingCLI();
      $res = $this->watch('.', false);
      if (!$this->isUsingCLI()) {
        $this->assertEqual(
          'unable to resolve root .: path "." must be absolute',
          idx($res, 'error')
        );
      } else {
        $this->assertEqual(
          $root,
          idx($res, 'watch')
        );
      }
    } catch (Exception $e) {
      chdir($here);
      throw $e;
    }

    chdir($here);
  }

  function testSlash() {
    $res = $this->watch('/', false);
    $this->assertEqual(
      'unable to resolve root /: cannot watch "/"',
      idx($res, 'error')
    );
  }
}
