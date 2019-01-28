#!/usr/bin/env python2
# This is run from the appveyor scripts to locate the dynamic deps
# and copy them in to the destination dir
import os
import re
import shutil
import subprocess
import sys
import glob

# 'dumpbin.exe' /nologo /dependents wez-watchman.exe
EXAMPLE_OUTPUT = """

Dump of file wez-watchman.exe

File Type: EXECUTABLE IMAGE

  Image has the following dependencies:

    glog.dll
    ADVAPI32.dll
    dbghelp.dll
    WS2_32.dll
    boost_context-vc141-mt-x64-1_67.dll
    boost_thread-vc141-mt-x64-1_67.dll
    double-conversion.dll
    KERNEL32.dll
    MSVCP140.dll
    VCRUNTIME140.dll
    api-ms-win-crt-runtime-l1-1-0.dll
    api-ms-win-crt-heap-l1-1-0.dll
    api-ms-win-crt-environment-l1-1-0.dll
    api-ms-win-crt-string-l1-1-0.dll
    api-ms-win-crt-time-l1-1-0.dll
    api-ms-win-crt-stdio-l1-1-0.dll
    api-ms-win-crt-convert-l1-1-0.dll
    api-ms-win-crt-math-l1-1-0.dll
    api-ms-win-crt-locale-l1-1-0.dll
    api-ms-win-crt-filesystem-l1-1-0.dll

  Summary

       FF000 .data
       2E000 .pdata
      115000 .rdata
        2000 .reloc
        1000 .rsrc
      2B8000 .text
"""

SYSTEM_BLACKLIST = set(
    [
        "advapi32.dll",
        "dbghelp.dll",
        "kernel32.dll",
        "msvcp140.dll",
        "vcruntime140.dll",
        "ws2_32.dll",
        "ntdll.dll",
        "shlwapi.dll",
    ]
)


def is_win():
    return sys.platform.startswith("win")


def is_system_dep(name):
    if name in SYSTEM_BLACKLIST:
        return True
    if "api-ms-win-crt" in name:
        return True
    return False


class State(object):
    def __init__(self, inst_dir, search_path, mock=False):
        self._inst_dir = inst_dir
        self._search_path = search_path
        self._mock = mock
        self._copied = set()
        self._dumpbin = None
        self._path = os.environ["PATH"].split(os.pathsep)

    def find_dumpbin(self):
        if self._dumpbin:
            return self._dumpbin

        # This is a little brittle, but there aren't many easy
        # options with locating this tool
        globs = [
            (
                "c:/Program Files (x86)/"
                "Microsoft Visual Studio/"
                "*/BuildTools/VC/Tools/"
                "MSVC/*/bin/Hostx64/x64/dumpbin.exe"
            ),
            (
                "c:/Program Files (x86)/"
                "Microsoft Visual Studio/"
                "*/Community/VC/Tools/"
                "MSVC/*/bin/Hostx64/x64/dumpbin.exe"
            ),
            (
                "c:/Program Files (x86)/"
                "Common Files/"
                "Microsoft/Visual C++ for Python/*/"
                "VC/bin/dumpbin.exe"
            ),
            ("c:/Program Files (x86)/Microsoft Visual Studio */VC/bin/dumpbin.exe"),
        ]
        for pattern in globs:
            for exe in glob.glob(pattern):
                self._dumpbin = exe
                return exe

        raise RuntimeError("could not find dumpbin.exe")

    def list_dynamic_deps(self, exe):
        if not self._mock:
            print("Resolve deps for %s" % exe)
            output = subprocess.check_output(
                [self.find_dumpbin(), "/nologo", "/dependents", exe]
            ).decode("utf-8")
        else:
            output = EXAMPLE_OUTPUT

        lines = output.split("\n")
        for line in lines:
            m = re.match("\s+(\S+.dll)", line, re.IGNORECASE)
            if m:
                dep = m.group(1).lower()
                yield dep

    def copy_to_inst_dir(self, shared_object):
        dest_name = os.path.join(self._inst_dir, os.path.basename(shared_object))
        if dest_name in self._copied:
            return False
        if not self._mock:
            print("Copying %s" % shared_object)
            shutil.copy(shared_object, dest_name)
        self._copied.add(dest_name)
        return dest_name

    def resolve_dep(self, depname):
        for d in self._search_path:
            name = os.path.join(d, depname)
            if self._mock:
                return name
            if os.path.exists(name):
                return name
        if self.resolve_dep_from_path(depname):
            # It's a system dep, so skip it
            return None

        message = "unable to find %s in %r" % (depname, self._search_path + self._path)
        print(message)
        if False:
            raise RuntimeError(message)
        return None

    def resolve_dep_from_path(self, depname):
        """ If we can find the dep in the path, then we consider it to
        be a system dependency that we should not bundle in the package """
        if is_system_dep(depname):
            return True

        for d in self._path:
            name = os.path.join(d, depname)
            if os.path.exists(name):
                return True

        return False

    def copy_deps(self, shared_object):
        print("[copy_deps %s]" % shared_object)
        for dep in self.list_dynamic_deps(shared_object):
            dep = self.resolve_dep(dep)
            if dep:
                target = self.copy_to_inst_dir(dep)
                if target is not False:
                    # If we just copied it, let's recurse into its deps
                    self.copy_deps(target)
                    print("[back to %s]" % shared_object)


if len(sys.argv) < 3:
    raise RuntimeError("copy-dyn-deps.py SRC_EXE INST_DIR [DLLDIRS]*")

EXE = sys.argv[1]
INST_DIR = sys.argv[2]
SEARCH_PATH = sys.argv[3:]

state = State(INST_DIR, SEARCH_PATH, mock=not is_win())
state.copy_deps(EXE)
print(state._copied)
