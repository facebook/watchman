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

class basicTestCase extends WatchmanTestCase {

  function testSomething() {
    $root = realpath($this->getRoot());

    $out = $this->watchmanCommand('watch', $root);
    $this->assertEqual($out['watch'], $root);

    $out = $this->watchmanCommand('find', $root, '*.c');
    $hash_ent = null;
    foreach ($out['files'] as $ent) {
      if ($ent['name'] == 'hash.c') {
        $hash_ent = $ent;
        break;
      }
    }
    $this->assertEqual($hash_ent['name'], 'hash.c');
    $this->assertEqual($hash_ent['exists'], true);
  }
}

// vim:ts=2:sw=2:et:

