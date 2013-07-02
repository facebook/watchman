<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class bigPCRETestCase extends WatchmanTestCase {

  function testBigPCRE() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $this->watch($root);

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

    // Some libraries will happily parse this big pcre
    if (isset($res['error'])) {
      $possible = array(
        'code 50 repeated subpattern is too long',
        'code 20 regular expression is too large',
      );
      $matched = false;
      foreach ($possible as $frag) {
        if (preg_match("/^failed to parse query: invalid pcre: ".
          "$frag at offset \d+/", $res['error'])) {
            $matched = true;
          }
      }

      $this->assertEqual(true, $matched, "got useful message: ".
        substr($res['error'], 0, 128));
    }
  }
}

// vim:ts=2:sw=2:et:
