/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
/* Primitive `makedepend` style utility.
 * Relies on you generating a .dep file using a compiler invocation like:
 * $(CC) $(CFLAGS) /Zs /showIncludes /EHsc $< > $(<:.c=.dep)
 * which captures the list of include files used by a given source.
 * This script will parse that output and collect it together to generate
 * an NMake style dependency graph for the target object file.
 */
shell = WScript.CreateObject("WScript.Shell");
cwd = shell.CurrentDirectory.toLowerCase() + '\\';
fs = WScript.CreateObject("Scripting.FileSystemObject");

INC_FILE = '.windeps';
// Force dependencies on the make tooling, so that objects are regenerated
// when the build environment is changed
EXTRA_DEPS = [];//'winbuild\\Makefile', 'winbuild\\depends.js'];
REL_DIR = ".";

function parse_dep_stream(is, write_back) {
  var deps = [];
  var current_file = null;
  var depfile = null;

  var includePat = /^Note: including file:\s+(.*)/

  while (!is.AtEndOfStream) {
    var line = is.ReadLine();
    var m = includePat.exec(line);
    if (m) {
      if (depfile) {
        depfile.WriteLine(line);
      }
      var candidate = m[1];
      var prefix = candidate.substr(0, cwd.length).toLowerCase();
      if (prefix == cwd) {
        deps.push('"' + candidate.substr(cwd.length) + '"');
      }
    } else {
      var candidate_file = line;
      if (REL_DIR != ".") {
        candidate_file = REL_DIR + "\\" + candidate_file;
      }

      if (!fs.FileExists(candidate_file)) {
        // Some random error that we can ignore
        continue;
      }

      // Then it is probably the name of the source file
      if (depfile) {
        depfile.Close();
        depfile = null;
      }

      current_file = candidate_file;

      deps = [];
      for (var i in EXTRA_DEPS) {
        deps.push(EXTRA_DEPS[i]);
      }

      if (write_back) {
        var depfilename = current_file.replace(/.(c|cpp)$/, '.dep');
        depfile = fs.CreateTextFile(depfilename, true);
        depfile.WriteLine(current_file);
      }
    }
  }

  if (depfile) {
    depfile.Close();
  }

  return deps;
}

function parse_dep_file(srcfile) {
  var depfile = srcfile.replace(/\.(c|cpp)$/, '.dep');
  var objfile = srcfile.replace(/\.(c|cpp)$/, '.obj');

  if (!fs.FileExists(depfile)) {
    return null;
  }
  var f = fs.GetFile(depfile);
  var is = f.OpenAsTextStream(1, 0);
  var deps = parse_dep_stream(is);
  is.Close();

  // Construct the dependency line
  var depline = objfile + ': ' + deps.join(' ');

  return {
    src: srcfile,
    obj: objfile,
    line: depline
  };
}

function load_include_file() {
  var deps = {};
  if (!fs.FileExists(INC_FILE)) {
    return deps;
  }
  var f = fs.GetFile(INC_FILE);
  var is = f.OpenAsTextStream(1, 0);
  var r = /^(.*\.obj):\s+(.*)$/;
  while (!is.AtEndOfStream) {
    var line = is.ReadLine();
    var m = r.exec(line);
    if (!m) {
      continue;
    }
    var key = m[1];
    var deplist = m[2];
    if (key.length > 0 && deplist.length > 0) {
      deps[key] = deplist;
    }
  }
  is.Close();
  return deps;
}

function save_include_file(deps) {
  var of = fs.CreateTextFile(INC_FILE, true);
  for (var key in deps) {
    of.WriteLine(deps[key]);
    // Also generate dependency between .c and .dep file
    var depfile = key.replace(/\.obj$/, '.dep');
    of.WriteLine(depfile + ": " + key);
  }
  of.Close();
}

function print_deps(deps) {
  for (var key in deps) {
    WScript.Echo(key + " -> " + deps[key]);
  }
}

DEPS = load_include_file();

function read_deps_from_stdin() {
  if (WScript.Arguments.length != 2) {
    return false;
  }
  if (WScript.Arguments(0) != '--stdin') {
    return false;
  }
  REL_DIR = WScript.Arguments(1);

  parse_dep_stream(WScript.StdIn, true);

  return true;
}

var find_blacklist = [
  shell.CurrentDirectory + "\\a",
  shell.CurrentDirectory + "\\website",
];

function is_blacklisted_dir(dir) {
  var i = find_blacklist.length;
  while (i--) {
    if (find_blacklist[i] == dir) {
      return true;
    }
  }
  return false;
}

function find_c_files(dir) {
  if (is_blacklisted_dir(dir) || /\.(git|hg|svn)/.exec(dir)) {
    return;
  }
  var f = fs.GetFolder(dir);
  var en = new Enumerator(f.files);
  for (; !en.atEnd(); en.moveNext()) {
    var item = en.item();
    if (/\.(c|cpp)$/.exec(item)) {
      var rel = item.Path.substr(cwd.length);
      var dep = parse_dep_file(rel);
      if (dep) {
        DEPS[dep.obj] = dep.line;
      }
    }
  }
  en = new Enumerator(f.subfolders);
  for (; !en.atEnd(); en.moveNext()) {
    var item = en.item();
    find_c_files(item);
  }
}

if (read_deps_from_stdin()) {
  WScript.Quit(0);
} else {
  find_c_files(shell.CurrentDirectory);
  save_include_file(DEPS);
}
