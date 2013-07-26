<?php
/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

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
    return true;
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
      'prefix' => array(
        'help' => 'Specify the installation prefix for configure',
        'param' => 'prefix',
      ),
      'statedir' => array(
        'help' => 'Specify the statedir configure option',
        'param' => 'path',
      ),
      'config-file' => array(
        'help' => 'Specify a local file that contains the configuration that ' .
                  'you want to install to /etc/watchman.json. The contents ' .
                  'of this file will be copied to /etc/watchman.json when ' .
                  'the RPM is installed',
        'param' => 'path',
      ),
      'gimli' => array(
        'help' => 'Configure to run under the gimli monitor',
      ),
      'pcre' => array(
        'help' => 'Configure to use pcre',
      ),
      'release' => array(
        'help' => 'Override the default RPM release of 1',
        'param' => 'release'
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
    $requires = '';
    $release = $this->getArgument('release');
    if (!$release) {
      $release = 1;
    }
    $configureargs = implode(' ', $this->getArgument('configureargs'));

    if ($this->getArgument('gimli')) {
      $configureargs .= ' --with-gimli';
      $requires = 'Requires: fb-gimli';
    }

    if ($this->getArgument('pcre')) {
      $configureargs .= ' --with-pcre';
      $requires = 'Requires: pcre';
    }

    $statedir = $this->getArgument('statedir');
    if ($statedir) {
      $configureargs .= ' --enable-statedir=' . $statedir;
      $files .= "%dir %attr(777, root, root) $statedir\n";
      $files .= "%dir %attr(777, root, root) $statedir/traces\n";
      $build .= "mkdir -p $root/ROOT/$statedir/traces\n";
    }

    $config_file = $this->getArgument('config-file');
    if ($config_file) {
      $config_file = realpath($config_file);
      $files .= "%config /etc/watchman.json\n";
      $build .= "mkdir -p $root/ROOT/etc\n";
      $build .= "cp $config_file $root/ROOT/etc/watchman.json\n";
    }

    $api = $this->getRepositoryAPI();
    $commit = $api->getSourceControlSystemName() . ':';
    $commit .= $api->getCanonicalRevisionName($api->getWorkingCopyRevision());
    $configureargs .= " --with-buildinfo=" . escapeshellarg($commit);

    $prefix = $this->getArgument('prefix', '/usr/local');

    $spec = "
%define _prefix $prefix
Name: fb-watchman
Version: $version
Release: $release
Summary: Watch files, trigger stuff
$requires

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

%post
# -9 will cause monitor to respawn them for users, so we'll restart
# and recrawl at upgrade time and not defer it until our users ask
# questions of the server
pkill -9 watchman

%preun
# We want it to go away; this normal termination will cause monitor
# to exit and we're done.
pkill watchman

%files
%defattr(-,root,root,-)
$prefix/bin/watchman
$prefix/share/doc/watchman-*
$files
";

    file_put_contents("$root/SPEC/watchman.spec", $spec);

    echo "Building package $configureargs\n";
    list($err, $stdout, $stderr) = exec_manual(
      'rpmbuild -bb --nodeps -vv --define "_topdir %C" %s',
      $root, "$root/SPEC/watchman.spec");
    if ($err) {
      echo "$stdout\n$stderr\n";
      throw new Exception("failed to build");
    }

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

