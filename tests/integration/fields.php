<?php
/* Copyright 2013-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// intval of a large double overflows 32-bit ints on win32 php
function stringy_intval($val) {
  if (gettype($val) == 'double') {
    $val = floor($val);
  }
  $val = (string)$val;
  return preg_replace("/\..*$/", '', $val);
}

class fieldsTestCase extends WatchmanTestCase {
  // We can't divide by 1000 here because that involves converting to float
  // which might lose too much precision.
  // On some systems, the json representation already converted to double,
  // so we have to handle that case too
  private function divideTimeBy1000($value) {
    if (gettype($value) == 'double') {
      $value /= 1000;
      return (string)floor($value);
    }
    return substr("$value", 0, -3);
  }

  function assertTimeEqual($expected, $actual, $actual_ms, $actual_us,
                           $actual_ns, $actual_f) {
    $this->assertEqual($expected, $actual, "seconds");

    $this->assertEqual("$actual", $this->divideTimeBy1000($actual_ms), "ms");
    $this->assertEqual("$actual_ms", $this->divideTimeBy1000($actual_us), "us");
    $this->assertEqual("$actual_us", $this->divideTimeBy1000($actual_ns), "ns");

    // Can't (int) cast because that yields bogus results on win32 php
    $this->assertEqual("$actual_ms", stringy_intval($actual_f * 1000), "float");
  }

  function testFields() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
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

    $compare_fields = array('size', 'mode', 'uid', 'gid', 'nlink');
    if (!phutil_is_windows()) {
      // These are meaningless in msvcrt, so no sense in comparing them
      $compare_fields[] = 'dev';
      $compare_fields[] = 'ino';
    }
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
