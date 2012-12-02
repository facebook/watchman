<?php

/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

class WatchmanTestCase extends ArcanistPhutilTestCase {
  private $watchman = null;
  protected $root;

  // because setProjectRoot is final and $this->projectRoot
  // is private...
  function setRoot($root) {
    $this->setProjectRoot($root);
    $this->root = $root;
  }

  function getRoot() {
    return $this->root;
  }

  function watchmanCommand() {
    $args = func_get_args();

    if (!$this->watchman) {
      $this->watchman = new WatchmanInstance();
    }

    return call_user_func_array(
      array($this->watchman, 'resolveCommand'),
      $args);
  }

  function assertRegex($pattern, $subject, $message = null) {
    if (!preg_match($pattern, $subject)) {
      if (!$message) {
        $message = "Failed to assert that $subject matches $pattern";
      }
      $this->assertFailure($message);
    }
  }

}


// vim:ts=2:sw=2:et:

