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

class WatchmanPackageWorkflow extends ArcanistBaseWorkflow {
  public function getWorkflowName() {
    return 'package';
  }

  public function getCommandSynopses() {
    return phutil_console_format(<<<TXT
      **package** [version]
TXT
    );
  }

  public function getCommandHelp() {
    return phutil_console_format(<<<TXT
          Generates an RPM suitable for use at Facebook
TXT
    );
  }

  public function requiresConduit() {
    return false;
  }

  public function requiresRepositoryAPI() {
    return false;
  }

  public function requiresAuthentication() {
    return false;
  }

  public function getArguments() {
    return array(
      'version' => array(
        'help' => 'Specify the package version, else look in configure.ac',
        'param' => 'version',
      ),
      'statedir' => array(
        'help' => 'Specify the statedir configure option',
        'param' => 'path',
      ),
      '*' => 'configureargs',
    );
  }

  public function run() {
    $srcdir = dirname(__FILE__) . '/../../';

    $version = $this->getArgument('version');
    if ($version) {
      $version = $version;
    } else {
      // Match out the version number from configure
      $ac = file_get_contents("$srcdir/configure.ac");
      if (preg_match('/AC_INIT\(\[watchman\],\s*\[([^[]+)\]/',
          $ac, $matches)) {
        $version = $matches[1];
      }
    }
    echo "make rpm for $version\n";

    $dir = PhutilDirectoryFixture::newEmptyFixture();
    $root = $dir->getPath();
    mkdir("$root/RPMS");
    mkdir("$root/SPEC");
    mkdir("$root/ROOT");

    echo "Copying src files\n";
    execx('cp -r %s %s', $srcdir, "$root/BUILD");

    $files = '';
    $build = '';
    $configureargs = implode(' ', $this->getArgument('configureargs'));
    $statedir = $this->getArgument('statedir');
    if ($statedir) {
      $configureargs .= ' --enable-statedir=' . $statedir;
      $files = "%dir %attr(777, root, root) $statedir";
      $build = "mkdir -p $root/ROOT/$statedir";
    }

    $spec = <<<TXT
Name: watchman
Version: $version
Release: 1
Summary: Watch files, trigger stuff

#Autoprov: 0
#Autoreq: 0

Group: System Environment
License: Apache 2.0
BuildRoot: $root/ROOT

%description
Watch files, trigger stuff

%build
./autogen.sh
%configure $configureargs
%makeinstall
$build

%files
%defattr(-,root,root,-)
/usr/bin/watchman
$files
TXT;

    file_put_contents("$root/SPEC/watchman.spec", $spec);

    echo "Building package $configureargs\n";
    execx('rpmbuild -bb --nodeps -vv --define "_topdir %C" %s',
      $root, "$root/SPEC/watchman.spec");

    $rpms = id(new FileFinder("$root/RPMS"))
      ->withType('f')
      ->withSuffix('rpm')
      ->find();

    foreach ($rpms as $rpm) {
      printf("Create %s\n", basename($rpm));
      rename("$root/RPMS/$rpm", basename($rpm));
    }

    return 0;
  }
}

// vim:ts=2:sw=2:et:

