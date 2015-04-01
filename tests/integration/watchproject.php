<?php
/* Copyright 2015-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

class watchProjectTestCaseHelper extends WatchmanTestCase {
  function runProjectTests($tests, $touch_watchmanconfig = false) {
    foreach ($tests as $info) {
      list($touch, $expected_watch, $expect_rel, $expected_pass) = $info;
      $dir = PhutilDirectoryFixture::newEmptyFixture();
      $root = realpath($dir->getPath());

      mkdir("$root/a/b/c", 0777, true);
      touch("$root/$touch");
      if ($touch_watchmanconfig) {
        touch("$root/.watchmanconfig");
      }
      $res = $this->watchProject("$root/a/b/c");
      $err = idx($res, 'error');

      // Dump some info to make it easier to diagnose failures
      $label = json_encode($info) . " res=" . json_encode($res);

      if ($expected_watch === null) {
        $full_watch = $root;
      } else {
        $full_watch = "$root/$expected_watch";
      }

      if ($expected_pass) {
        if ($err) {
          $this->assertFailure("failed to watch-project: $err");
        }
        $this->assertEqual($full_watch, idx($res, 'watch'), $label);
        $this->assertEqual($expect_rel, idx($res, 'relative_path'), $label);
      } else {
        if ($err) {
          $this->assertEqual(
            "resolve_projpath: none of the files listed in global config ".
            "root_files are present in path `$root/a/b/c` or any ".
            "of its parent directories", $err, $label);
        } else {
          $this->assertFailure("didn't expect watch-project success $label");
        }
      }
    }
  }
}

class watchProjectTestCase extends watchProjectTestCaseHelper {
  function getGlobalConfig() {
    return array(
      'root_files' => array('.git', '.hg', '.foo', '.bar')
    );
  }

  function testWatchProject() {
    $this->runProjectTests(array(
      array("a/b/c/.git", "a/b/c", NULL, true),
      array("a/b/.hg", "a/b", "c", true),
      array("a/.foo", "a", "b/c", true),
      array(".bar", NULL, "a/b/c", true),
      array("a/.bar", "a", "b/c", true),
      array(".svn", "a/b/c", NULL, true),
      array("a/baz", "a/b/c", NULL, true),
    ));
  }

  function testWatchProjectWatchmanConfig() {
    $this->runProjectTests(array(
      array("a/b/c/.git", NULL, "a/b/c", true),
      array("a/b/.hg", NULL, "a/b/c", true),
      array("a/.foo", NULL, "a/b/c", true),
      array(".bar", NULL, "a/b/c", true),
      array("a/.bar", NULL, "a/b/c", true),
      array(".svn", NULL, "a/b/c", true),
      array("a/baz", NULL, "a/b/c", true),
    ), true);
  }
}

class watchProjectEnforcingTestCase extends watchProjectTestCaseHelper {
  function getGlobalConfig() {
    return array(
      'root_files' => array('.git', '.hg', '.foo', '.bar'),
      'enforce_root_files' => true,
    );
  }

  function testWatchProjectEnforcing() {
    $this->runProjectTests(array(
      array("a/b/c/.git", "a/b/c", NULL, true),
      array("a/b/.hg", "a/b", "c", true),
      array("a/.foo", "a", "b/c", true),
      array(".bar", NULL, "a/b/c", true),
      array("a/.bar", "a", "b/c", true),
      array(".svn", NULL, NULL, false),
      array("a/baz", NULL, NULL, false),
    ));
  }
}
