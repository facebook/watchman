<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class basicTestCase extends WatchmanTestCase {

  function testPCRE() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array('bar.txt', 'foo.c'));

    $out = $this->watchmanCommand('find', $root, '-p', '.*c$');
    $this->assertEqual('foo.c', $out['files'][0]['name']);

    $out = $this->watchmanCommand('find', $root, '-p', '.*txt$');
    $this->assertEqual('bar.txt', $out['files'][0]['name']);

    // Cleanup for invalid pcre
    $out = $this->watchmanCommand('find', $root, '-p', '(');
    $this->assertEqual(
      "invalid rule spec: invalid pcre: `(' at offset 1: code 14 missing )",
      $out['error']
    );

    // Test case insensitive mode
    $out = $this->watchmanCommand('find', $root, '-P', '.*C$');
    $this->assertEqual('foo.c', $out['files'][0]['name']);
  }

  function testFind() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/foo.c");
    touch("$root/bar.txt");

    $out = $this->watchmanCommand('watch', $root);
    $this->assertEqual($root, $out['watch']);

    $this->assertFileList($root, array('bar.txt', 'foo.c'));

    // Make sure we correctly observe deletions
    $this->assertEqual(true, unlink("$root/bar.txt"));
    $this->assertFileList($root, array('foo.c'));

    // touch -> delete -> touch, should show up as exists
    $this->assertEqual(true, touch("$root/bar.txt"));
    $this->assertFileList($root, array('bar.txt', 'foo.c'));

    $this->assertEqual(true, unlink("$root/bar.txt"));

    // A moderately more complex set of changes
    $this->assertEqual(true, mkdir("$root/adir"));
    $this->assertEqual(true, mkdir("$root/adir/subdir"));
    $this->assertEqual(true, touch("$root/adir/subdir/file"));
    $this->assertEqual(true,
      rename("$root/adir/subdir", "$root/adir/overhere"));

    $this->assertFileList($root, array(
      'adir',
      'adir/overhere',
      'adir/overhere/file',
      'foo.c',
    ));

    $this->assertEqual(true,
      rename("$root/adir", "$root/bdir"));

    $this->assertFileList($root, array(
      'bdir',
      'bdir/overhere',
      'bdir/overhere/file',
      'foo.c',
    ));
  }

  function testTwoDeep() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array());

    $this->assertEqual(true, mkdir("$root/foo"));
    $this->assertEqual(true, mkdir("$root/foo/bar"));
    $this->assertEqual(3, file_put_contents("$root/foo/bar/111", "111"));

    $this->assertFileList($root, array(
      "foo",
      "foo/bar",
      "foo/bar/111"
    ));

    execx('rm -rf %s', "$root/foo/bar");

    $this->assertFileList($root, array(
      "foo",
    ));
  }

  function testSinceIssue2() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);
    $this->assertFileList($root, array());

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo');
    mkdir("$root/foo");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/111');
    touch("$root/foo/111");

    $this->assertFileListUsingSince($root, 'n:foo', array(
      'foo',
      'foo/111',
    ), null);

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo/bar');
    mkdir("$root/foo/bar");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/bar/222');
    touch("$root/foo/bar/222");

    $this->watchmanCommand('log', 'debug', 'XXX: wait to observe lists');
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/111',
        'foo/bar',
        'foo/bar/222'
      ), array(
        'foo/bar',
        'foo/bar/222'
      )
    );
    $this->watchmanCommand('log', 'debug', 'XXX: closing out');

  }

  function testSinceIssue1() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    touch("$root/111");
    touch("$root/222");

    $watch = $this->watchmanCommand('watch', $root);
    $this->assertFileList($root, array(
      '111',
      '222'
    ));

    $initial = $this->watchmanCommand('since', $root, 'n:foo');

    mkdir("$root/bar");
    touch("$root/bar/333");

    // We should not observe 111 or 222 in this update
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        '111',
        '222',
        'bar',
        'bar/333'
      ),
      array(
        'bar',
        'bar/333'
      )
    );
  }

  function testMoveReAdd() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    mkdir("$root/foo");
    $watch = $this->watchmanCommand('watch', $root);

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo'
      ),
      array(
        'foo'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/222');
    touch("$root/foo/222");
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/222',
      ),
      array(
        'foo/222'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo/bar');
    mkdir("$root/foo/bar");
    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        'foo',
        'foo/222',
        'foo/bar',
      ),
      array(
        'foo/bar'
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo/bar');
    rmdir("$root/foo/bar");
    $this->watchmanCommand('log', 'debug', 'XXX: unlink foo/222');
    unlink("$root/foo/222");
    $this->watchmanCommand('log', 'debug', 'XXX: rmdir foo');
    rmdir("$root/foo");

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
      ),
      array(
      )
    );

    $this->watchmanCommand('log', 'debug', 'XXX: mkdir foo');
    mkdir("$root/foo");
    $this->watchmanCommand('log', 'debug', 'XXX: touch foo/222');
    touch("$root/foo/222");

    $this->assertFileListUsingSince($root, 'n:foo',
      array(
        "foo",
        "foo/222",
      ),
      array(
        "foo",
        "foo/222",
      )
    );
  }

  function testRemove() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    mkdir("$root/one");
    touch("$root/one/onefile");
    mkdir("$root/one/two");
    touch("$root/one/two/twofile");
    touch("$root/top");

    $this->watchmanCommand('watch', $root);
    $this->assertFileList($root, array(
      'one',
      'one/onefile',
      'one/two',
      'one/two/twofile',
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: remove dir one');
    Filesystem::remove("$root/one");

    $this->assertFileList($root, array(
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: touch file one');
    touch("$root/one");
    $this->assertFileList($root, array(
      'one',
      'top'
    ));

    $this->watchmanCommand('log', 'debug', 'XXX: unlink file one');
    unlink("$root/one");
    $this->assertFileList($root, array(
      'top'
    ));
  }

  // Verify that we don't generate spurious change observations
  // when we delete files at the top level
  function testBSDishTopLevel() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());

    mkdir("$root/lower");
    touch("$root/lower/file");
    touch("$root/top");

    $this->watchmanCommand('watch', $root);

    $this->assertFileList($root, array(
      'lower',
      'lower/file',
      'top'
    ));

    $find = $this->watchmanCommand('find', $root);
    $clock = $find['clock'];

    $since = $this->watchmanCommand('since', $root, $clock);
    $clock = $since['clock'];

    $since = $this->watchmanCommand('since', $root, $clock);
    $this->assertEqual(array(), $since['files']);
    $clock = $since['clock'];

    unlink("$root/top");

    $this->assertFileList($root, array(
      'lower',
      'lower/file'
    ));

    $now = $this->watchmanCommand('since', $root, $clock);

    //print_r($now);
    $this->assertEqual(1, count($now['files']));
    $this->assertEqual('top', $now['files'][0]['name']);
    $this->assertEqual(false, $now['files'][0]['exists']);
  }

  function testCursor() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watchmanCommand('watch', $root);

    $initial = $this->watchmanCommand('since', $root,
      'n:testCursor');

    $this->assertRegex('/^c:\d+:\d+$/', $initial['clock'],
      "clock seemslegit");

    touch($root . '/one');

    // Allow time for the change to be observed
    $update = $this->waitForWatchman(
      array('since', $root, 'n:testCursor'),
      function ($update) {
        return count($update['files']);
      }
    );

    $this->assertEqual('one',
      $update['files'][0]['name'], 'saw file change');
    $this->assertEqual(true,
      $update['files'][0]['new'], 'shows as new');

    $later = $this->watchmanCommand('since', $root,
      'n:testCursor');
    $this->assertEqual(array(), $later['files'], 'no changes');

    /* now to verify that the file doesn't show as new after this next
     * change */
    touch($root . '/one');

    // Allow time for the change to be observed
    $update = $this->waitForWatchman(
      array('since', $root, 'n:testCursor'),
      function ($update) {
        return count($update['files']);
      }
    );

    $this->assertEqual('one',
      $update['files'][0]['name'], 'saw file change');
    $this->assertEqual(false,
      isset($update['files'][0]['new']), 'not new');

  }
}

// vim:ts=2:sw=2:et:

