<?php
/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class triggerErrorsTestCase extends WatchmanTestCase {
  function assertTriggerRegError($err, $args) {
    $args = func_get_args();
    array_shift($args);
    $out = call_user_func_array(array($this, 'watchmanCommand'), $args);
    $this->assertEqual($err, $out['error']);
  }

  function testBadArgs() {
    $dir = new WatchmanDirectoryFixture();
    $root = $dir->getPath();
    $this->watch($root);

    $this->assertTriggerRegError('wrong number of arguments', 'trigger');

    $this->assertTriggerRegError(
      'not enough arguments',
      'trigger', $root
    );

    $this->assertTriggerRegError(
      'no command was specified',
      'trigger', $root, 'oink'
    );

    $this->assertTriggerRegError(
      'no command was specified',
      'trigger', $root, 'oink', '--'
    );

    $this->assertTriggerRegError(
      'failed to parse query: rule @ position 4 is not a string value',
      'trigger', $root, 'oink', '--', 123
    );

    $this->assertTriggerRegError(
      'invalid or missing name',
      'trigger', $root, 123
    );

    $this->assertTriggerRegError(
      'invalid or missing name',
      'trigger', $root, array(
      )
    );

    $this->assertTriggerRegError(
      'invalid or missing name',
      'trigger', $root, array(
        'name' => 123,
      )
    );

    $this->assertTriggerRegError(
      'invalid command array',
      'trigger', $root, array(
        'name' => 'oink',
      )
    );

    $this->assertTriggerRegError(
      'invalid command array',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => 123,
      )
    );

    $this->assertTriggerRegError(
      'invalid command array',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array(),
      )
    );

    $this->assertTriggerRegError(
      'invalid stdin value lemon',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array('cat'),
        'stdin' => 'lemon',
      )
    );

    $this->assertTriggerRegError(
      'invalid value for stdin',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array('cat'),
        'stdin' => 13
      )
    );

    $this->assertTriggerRegError(
      'max_files_stdin must be >= 0',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array('cat'),
        'max_files_stdin' => -1,
      )
    );

    $this->assertTriggerRegError(
      'stdout: must be prefixed with either > or >>, got out',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array('cat'),
        'stdout' => "out",
      )
    );

    $this->assertTriggerRegError(
      'stderr: must be prefixed with either > or >>, got out',
      'trigger', $root, array(
        'name' => 'oink',
        'command' => array('cat'),
        'stderr' => "out",
      )
    );
  }

}
