#!/usr/bin/env python
#
# Copyright (c) 2004-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from __future__ import absolute_import, division, print_function, unicode_literals

import argparse
import os
import shlex
import subprocess
import sys


try:
    from shlex import quote as shellquote
except ImportError:
    from pipes import quote as shellquote


class BuildOptions(object):
    def __init__(self, num_jobs, external_dir, install_dir):
        self.num_jobs = num_jobs
        if not self.num_jobs:
            import multiprocessing

            self.num_jobs = multiprocessing.cpu_count()

        self.external_dir = external_dir
        if install_dir is None:
            install_dir = os.path.join(self.external_dir, "install")
        self.install_dir = install_dir

    def project_dir(self, name, *paths):
        return os.path.join(self.external_dir, name, *paths)


class Project(object):
    def __init__(self, name, opts, updater, builder):
        self.name = name
        self.opts = opts
        self.updater = updater
        self.builder = builder
        self.path = self.opts.project_dir(self.name)

    def update(self):
        self.updater.update(self)

    def ensure_checkedout(self):
        self.updater.ensure_checkedout(self)

    def build(self):
        self.builder.build(self)

    def clean(self):
        self.updater.clean(self)


class GitUpdater(object):
    def __init__(self, repo, branch="master"):
        self.origin_repo = repo
        self.branch = branch

    def ensure_checkedout(self, project):
        if not os.path.exists(project.path):
            self._checkout(project)

    def update(self, project):
        if os.path.exists(project.path):
            print("Updating %s..." % project.name)
            run_cmd(["git", "-C", project.path, "fetch", "origin"])
            run_cmd(
                [
                    "git",
                    "-C",
                    project.path,
                    "merge",
                    "--ff-only",
                    "origin/%s" % self.branch,
                ]
            )
        else:
            self._checkout(project)

    def _checkout(self, project):
        print("Cloning %s..." % project.name)
        run_cmd(
            [
                "git",
                "clone",
                "--depth=100",
                self.origin_repo,
                project.path,
                "--branch",
                self.branch,
            ]
        )

    def clean(self, project):
        run_cmd(["git", "-C", project.path, "clean", "-fxd"])


def homebrew_prefix():
    """ returns the homebrew installation prefix """
    return subprocess.check_output(["brew", "--prefix"]).strip()


def fixup_env_for_darwin(env):
    def add_flag(name, item, separator, append=True):
        val = env.get(name, "").split(separator)
        if append:
            val.append(item)
        else:
            val.insert(0, item)
        env[name] = separator.join(val)

    # The brew/openssl installation situation is a bit too weird for vanilla
    # cmake logic to find, and most packages don't deal with this correctly,
    # so inject these into the environment to give them a hand
    p = homebrew_prefix()
    add_flag("PKG_CONFIG_PATH", "%s/opt/openssl/lib/pkgconfig" % p, ":")
    add_flag("LDFLAGS", "-L%s/opt/openssl/lib" % p, " ")
    add_flag("CPPFLAGS", "-I%s/opt/openssl/include" % p, " ")

    # system bison is ancient, so ensure that the brew installed one takes
    # precedence.  Brew refuses to to install or link bison into /usr/local/bin,
    # so we have to insert this opt path instead.  Likewise for flex.
    add_flag("PATH", "%s/opt/bison/bin" % p, ":", append=False)
    add_flag("PATH", "%s/opt/flex/bin" % p, ":", append=False)

    # flex generates code that sprinkles the `register` keyword liberally
    # and the thrift compilation flags hate that in C++17 code.  Disable
    # the warning that promotes this to an error.
    add_flag("CXXFLAGS", "-Wno-register", " ")


class BuilderBase(object):
    def __init__(self, subdir=None, env=None, build_dir=None):
        self.env = os.environ.copy()
        if sys.platform == "darwin":
            fixup_env_for_darwin(self.env)
        if env:
            self.env.update(env)

        self.subdir = subdir
        self.build_dir = build_dir
        self._build_path = None

    def _run_cmd(self, cmd):
        run_cmd(cmd=cmd, env=self.env, cwd=self._build_path)

    def build(self, project):
        print("Building %s..." % project.name)
        if self.subdir:
            build_path = os.path.join(project.path, self.subdir)
        else:
            build_path = project.path

        if self.build_dir is not None:
            build_path = os.path.join(build_path, self.build_dir)
            if not os.path.isdir(build_path):
                os.mkdir(build_path)

        self._build_path = build_path
        try:
            self._build(project)
        finally:
            self._build_path = None


class MakeBuilder(BuilderBase):
    def __init__(self, subdir=None, env=None, args=None):
        super(MakeBuilder, self).__init__(subdir=subdir, env=env)
        self.args = args or []

    def _build(self, project):
        cmd = ["make", "-j%s" % project.opts.num_jobs] + self.args
        self._run_cmd(cmd)

        install_cmd = ["make", "install", "PREFIX=" + project.opts.install_dir]
        self._run_cmd(install_cmd)


class AutoconfBuilder(BuilderBase):
    def __init__(self, subdir=None, env=None, args=None):
        super(AutoconfBuilder, self).__init__(subdir=subdir, env=env)
        self.args = args or []

    def _build(self, project):
        configure_path = os.path.join(self._build_path, "configure")
        if not os.path.exists(configure_path):
            self._run_cmd(["autoreconf", "--install"])
        configure_cmd = [
            configure_path,
            "--prefix=" + project.opts.install_dir,
        ] + self.args
        self._run_cmd(configure_cmd)
        self._run_cmd(["make", "-j%s" % project.opts.num_jobs])
        self._run_cmd(["make", "install"])


class CMakeBuilder(BuilderBase):
    def __init__(self, subdir=None, env=None, defines=None):
        super(CMakeBuilder, self).__init__(subdir=subdir, env=env, build_dir="_build")
        self.defines = defines or {}

    def _build(self, project):
        defines = {
            "CMAKE_INSTALL_PREFIX": project.opts.install_dir,
            "BUILD_SHARED_LIBS": "OFF",
            # Some of the deps (rsocket) default to UBSAN enabled if left
            # unspecified.  Some of the deps fail to compile in release mode
            # due to warning->error promotion.  RelWithDebInfo is the happy
            # medium.
            "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        }

        # If any of these env vars are set, set the corresponding cmake def.
        for e in [
            "OPENSSL_ROOT_DIR",
            "BOOST_ROOT",
            "LIBEVENT_INCLUDE_DIR",
            "LIBEVENT_LIB",
            "CMAKE_SYSTEM_PREFIX_PATH",
        ]:
            var = os.environ.get(e, None)
            if var:
                defines[e] = var

        if is_win():
            defines["CMAKE_TOOLCHAIN_FILE"] = (
                vcpkg_dir() + "/scripts/buildsystems/vcpkg.cmake"
            )

        defines.update(self.defines)
        define_args = ["-D%s=%s" % (k, v) for (k, v) in defines.items()]

        if is_win():
            define_args += ["-G", "Visual Studio 15 2017 Win64"]

        self._run_cmd(["cmake", "configure", ".."] + define_args)

        if is_win():
            self._run_cmd(
                [
                    "cmake",
                    "--build",
                    self._build_path,
                    "--target",
                    "install",
                    "--config",
                    "Release",
                    # With a sufficiently new cmake available, we could
                    # ask for concurrency, but for now we don't have it.
                    # "-j", str(project.opts.num_jobs),
                ]
            )
        else:
            # The only thing stopping us from using the same cmake --build
            # approach as above is that the cmake that ships with ubuntu 16
            # is too old and doesn't know about the -j flag, so we do this
            # bit the old fashioned way
            self._run_cmd(["make", "-j%s" % project.opts.num_jobs])
            self._run_cmd(["make", "install"])


def run_cmd(cmd, env=None, cwd=None, allow_fail=False):
    cmd_str = " ".join(shellquote(arg) for arg in cmd)
    print("+ " + cmd_str)
    if allow_fail:
        subprocess.call(cmd, env=env, cwd=cwd)
    else:
        subprocess.check_call(cmd, env=env, cwd=cwd)


def install_apt(pkgs):
    cmd = ["sudo", "apt-get", "install", "-yq"] + pkgs
    run_cmd(cmd)


def vcpkg_dir():
    """ Figure out where vcpkg is installed.
    vcpkg-exported is populated in some flavors of FB internal builds.
    C:/tools/vcpkg is the appveyor location.
    C:/open/vcpkg is my local location.
    """
    for p in ["vcpkg-exported", "C:/tools/vcpkg", "C:/open/vcpkg"]:
        if os.path.isdir(p):
            return os.path.realpath(p)
    raise Exception("cannot find vcpkg")


def install_vcpkg(pkgs):
    vcpkg = os.path.join(vcpkg_dir(), "vcpkg")
    run_cmd([vcpkg, "install", "--triplet", "x64-windows"] + pkgs)


def get_projects(opts):
    projects = [
        Project(
            "mstch",
            opts,
            GitUpdater("https://github.com/no1msd/mstch.git"),
            CMakeBuilder(),
        ),
        Project(
            "googletest",
            opts,
            GitUpdater("https://github.com/google/googletest.git"),
            CMakeBuilder(),
        ),
    ]

    if not is_win():
        # Ubuntu 16 has an old version of zstd, so build our own.
        # We can't use the MakeBuilder on windows, but we can get zstd
        # from vcpkg so we're ok there.
        projects += [
            Project(
                "zstd",
                opts,
                GitUpdater("https://github.com/facebook/zstd.git"),
                MakeBuilder(),
            )
        ]

    projects += [
        # TODO: see if we can get get a faster and/or static build working
        # by building things ourselves.
        #        Project(
        #            "libevent",
        #            opts,
        #            GitUpdater("https://github.com/libevent/libevent.git",
        #                branch="release-2.1.8-stable"),
        #            CMakeBuilder(defines={
        #                'EVENT__DISABLE_BENCHMARK': 'ON',
        #                'EVENT__DISABLE_TESTS': 'ON',
        #                'EVENT__DISABLE_REGRESS': 'ON',
        #                'EVENT__DISABLE_SAMPLES': 'ON',
        #                }),
        #        ),
        Project(
            "folly",
            opts,
            GitUpdater("https://github.com/facebook/folly.git"),
            CMakeBuilder(),
        )
    ]

    need_thrift = not is_win()
    if need_thrift:
        projects += [
            Project(
                "libsodium",
                opts,
                GitUpdater("https://github.com/jedisct1/libsodium.git"),
                AutoconfBuilder(args=["--disable-shared"]),
            ),
            Project(
                "fizz",
                opts,
                GitUpdater("https://github.com/facebookincubator/fizz.git"),
                CMakeBuilder(
                    subdir="fizz",
                    defines={"BUILD_EXAMPLES": "OFF", "BUILD_TESTS": "OFF"},
                ),
            ),
            Project(
                "wangle",
                opts,
                GitUpdater("https://github.com/facebook/wangle.git"),
                CMakeBuilder(subdir="wangle", defines={"BUILD_TESTS": "OFF"}),
            ),
            Project(
                "rsocket-cpp",
                opts,
                GitUpdater("https://github.com/rsocket/rsocket-cpp.git"),
                CMakeBuilder(
                    defines={"BUILD_EXAMPLES": "OFF", "BUILD_BENCHMARKS": "OFF"}
                ),
            ),
            Project(
                "fbthrift",
                opts,
                GitUpdater("https://github.com/facebook/fbthrift.git"),
                CMakeBuilder(),
            ),
        ]
    return projects


def get_linux_type():
    try:
        with open("/etc/os-release") as f:
            data = f.read()
    except EnvironmentError:
        return (None, None)

    os_vars = {}
    for line in data.splitlines():
        parts = line.split("=", 1)
        if len(parts) != 2:
            continue
        key = parts[0].strip()
        value_parts = shlex.split(parts[1].strip())
        if not value_parts:
            value = ""
        else:
            value = value_parts[0]
        os_vars[key] = value

    return os_vars.get("NAME"), os_vars.get("VERSION_ID")


def get_os_type():
    if sys.platform.startswith("linux"):
        return get_linux_type()
    elif sys.platform.startswith("darwin"):
        return ("darwin", None)
    elif sys.platform.startswith("win"):
        return ("windows", sys.getwindowsversion().major)
    else:
        return (None, None)


def is_win():
    return get_os_type()[0] == "windows"


def install_platform_deps():
    os_name, os_version = get_os_type()
    if os_name is None:
        raise Exception("unable to detect OS type")
    elif os_name == "Centos" or os_name == "Fedora":
        print("Untested! Installing necessary packages...")
        # FIXME: make this work.  Contributions welcomed!
        pkgs = (
            "autoconf automake libdouble-conversion-dev "
            "libssl-dev make zip git libtool g++ libboost-all-dev "
            "libevent-dev flex bison libgoogle-glog-dev libkrb5-dev "
            "libsnappy-dev libsasl2-dev libnuma-dev libcurl4-gnutls-dev "
            "libpcap-dev libdb5.3-dev cmake pkg-config python-dev "
        ).split()
        run_cmd(["yum", "install"] + pkgs)
        raise Exception("implement me")
    elif os_name == "Ubuntu" or os_name.startswith("Debian"):
        # These dependencies have been tested on Ubuntu 16.04
        print("Installing necessary Ubuntu packages...")
        ubuntu_pkgs = (
            "autoconf automake libdouble-conversion-dev "
            "libssl-dev make zip git libtool g++ libboost-all-dev "
            "libevent-dev flex bison libgoogle-glog-dev libkrb5-dev "
            "libsnappy-dev libsasl2-dev libnuma-dev libcurl4-gnutls-dev "
            "libpcap-dev libdb5.3-dev cmake pkg-config python-dev "
            "libpcre3-dev "
        ).split()
        install_apt(ubuntu_pkgs)
    elif os_name == "windows":
        install_vcpkg(
            [
                "boost-chrono",
                "boost-context",
                "boost-conversion",
                "boost-crc",
                "boost-date-time",
                "boost-filesystem",
                "boost-multi-index",
                "boost-program-options",
                "boost-regex",
                "boost-system",
                "boost-thread",
                "boost-variant",
                "double-conversion",
                "gflags",
                "glog",
                "libevent",
                "openssl",
            ]
        )
    elif os_name == "darwin":
        print("Installing necessary packages via Homebrew...")
        run_cmd(
            [
                "brew",
                "install",
                "autoconf",
                "automake",
                "bison",
                "boost",
                "boost-python",
                "cmake",
                "curl",
                "double-conversion",
                "flex",
                "gflags",
                "glog",
                "libevent",
                "libgit2",
                "libtool",
                "lz4",
                "openssl",
                "snappy",
                "xz",
                "zstd",
            ],
            allow_fail=True,
        )

    else:
        raise Exception(
            "installing OS dependencies on %s is not supported yet" % (os_name,)
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "-o",
        "--external-dir",
        help="The directory where external projects should be "
        'created (default="external")',
    )
    ap.add_argument(
        "-u",
        "--update",
        action="store_true",
        default=False,
        help="Updates the external projects repositories before building them",
    )
    ap.add_argument(
        "-C",
        "--clean",
        action="store_true",
        default=None,
        help="Cleans the external project repositories before "
        "building them (defaults to on when updating projects)",
    )
    ap.add_argument(
        "--no-clean",
        action="store_false",
        default=None,
        dest="clean",
        help="Do not clean the external project repositories "
        "even after updating them.",
    )
    ap.add_argument(
        "-j",
        "--jobs",
        dest="num_jobs",
        type=int,
        default=None,
        help="The number of jobs to run in parallel when building",
    )
    ap.add_argument(
        "--install-dir",
        help="Directory where external projects should be "
        "installed (default=<external-dir>/install)",
    )
    ap.add_argument(
        "--install-deps",
        action="store_true",
        default=False,
        help="Install necessary system packages",
    )

    args = ap.parse_args()

    if args.external_dir is None:
        script_dir = os.path.abspath(os.path.dirname(__file__))
        args.external_dir = os.path.join(script_dir, "external")
    if args.clean is None:
        args.clean = args.update

    opts = BuildOptions(args.num_jobs, args.external_dir, args.install_dir)

    if args.install_deps:
        install_platform_deps()

    if not os.path.isdir(opts.external_dir):
        os.makedirs(opts.external_dir)

    projects = get_projects(opts)
    for project in projects:
        if args.update:
            project.update()
        else:
            project.ensure_checkedout()

    if args.clean:
        for project in projects:
            project.clean()

    for project in projects:
        project.build()


if __name__ == "__main__":
    main()
