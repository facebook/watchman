/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

// Super simple test driver

shell = WScript.CreateObject("WScript.Shell");
var success = true;

function run_test(name) {
  WScript.Echo("Running " + name);
  var res = shell.Run(name, 0, true);
  if (res != 0) {
    WScript.Echo(name + " exited with status " + res);
    success = false;
  }
}

args = WScript.Arguments;
for (i = 0; i < args.length; i++) {
  run_test(args(i));
}

WScript.Quit(success ? 0 : 1);
