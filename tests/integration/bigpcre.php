<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class bigPCRETestCase extends WatchmanTestCase {

  function testBigPCRE() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $this->watchmanCommand('watch', $root);

    $fill = str_repeat('lemon\\.php', 3600);
    $pcre = '^(' . chunk_split($fill, 100, '|') . 'sss)';
    $res = $this->watchmanCommand(
      'query',
      $root,
      array(
        'expression' => array(
          'pcre',
          $pcre,
          'wholename'
        ),
        'fields' => array(
          'name'
        )
      )
    );

    $this->assertEqual(1,
      preg_match('/^failed to parse query: invalid pcre: code 20 '.
        'regular expression is too large at offset \d+/',
        $res['error']
      ),
      'got useful error message'
    );
  }
}

// vim:ts=2:sw=2:et:
