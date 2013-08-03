<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class fieldsTestCase extends WatchmanTestCase {
  function assertTimeEqual($expected, $actual, $actual_ms, $actual_us,
                           $actual_ns, $actual_f) {
    $this->assertEqual($expected, $actual, "seconds");
    // We can't divide by 1000 here because that involves converting to float
    // which might lose too much precision
    $this->assertEqual("$actual", substr("$actual_ms", 0, -3), "ms");
    $this->assertEqual("$actual_ms", substr("$actual_us", 0, -3), "us");
    $this->assertEqual("$actual_us", substr("$actual_ns", 0, -3), "ns");

    $this->assertEqual($actual_ms, (int) ($actual_f * 1000), "float");
  }

  function testFields() {
    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = realpath($dir->getPath());
    $watch = $this->watch($root);
    $this->assertFileList($root, array());

    $this->watchmanCommand('log', 'debug', 'XXX: touch a');
    touch("$root/a");
    $this->assertFileList($root, array('a'));

    $query = $this->watchmanCommand('query', $root, array(
      'fields' => array('name', 'exists', 'new', 'size', 'mode', 'uid', 'gid',
                        'mtime', 'mtime_ms', 'mtime_us', 'mtime_ns', 'mtime_f',
                        'ctime', 'ctime_ms', 'ctime_us', 'ctime_ns', 'ctime_f',
                        'ino', 'dev', 'nlink', 'oclock', 'cclock'),
      'since' => 'n:foo'));

    $this->assertEqual(null, idx($query, 'error'));
    $this->assertEqual(1, count($query['files']));
    $file = $query['files'][0];
    $this->assertEqual('a', $file['name']);
    $this->assertEqual(true, $file['exists']);
    $this->assertEqual(true, $file['new']);

    $stat = stat("$root/a");

    $compare_fields = array('size', 'mode', 'uid', 'gid', 'ino', 'dev',
                            'nlink');
    foreach ($compare_fields as $field) {
        $this->assertEqual($stat[$field], $file[$field], $field);
    }

    $time_fields = array('mtime', 'ctime');
    foreach ($time_fields as $field) {
        $this->assertTimeEqual($stat[$field], $file[$field],
                               $file[$field . '_ms'], $file[$field . '_us'],
                               $file[$field . '_ns'], $file[$field . '_f']);
    }

    $this->assertRegex('/^c:\d+:\d+:\d+:\d+$/', $file['cclock'],
                       "cclock looks clocky");
    $this->assertRegex('/^c:\d+:\d+:\d+:\d+$/', $file['oclock'],
                       "oclock looks clocky");
  }
}

