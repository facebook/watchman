/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */
#include "watchman_system.h"
#include "ChildProcess.h"
#include "thirdparty/tap.h"

using watchman::ChildProcess;
using Environment = ChildProcess::Environment;
using Options = ChildProcess::Options;

void test_pipe() {
  Options opts;
  opts.pipeStdout();
  ChildProcess echo(
      {
#ifndef _WIN32
          "/bin/echo",
#else
          "cmd",
          "/c",
          "echo",
#endif
          "hello"},
      std::move(opts));

  echo.pipe(STDOUT_FILENO).read.clearNonBlock();
  echo.wait();

  char buf[128];
  auto len = read(echo.pipe(STDOUT_FILENO).read.fd(), buf, sizeof(buf));

  ok(len >= 0,
     "read from pipe was successful: len=%d err=%s",
     len,
     strerror(errno));
  w_string_piece line(buf, len);
  ok(line.startsWith("hello"), "starts with hello");
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(2);
  test_pipe();

  return exit_status();
}
