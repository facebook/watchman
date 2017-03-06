/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */
#include "watchman_system.h"
#include <list>
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

  auto outputs = echo.communicate();
  echo.wait();

  w_string_piece line(outputs.first);
  ok(line.startsWith("hello"), "starts with hello");
}

void test_pipe_input(bool threaded) {
#ifndef _WIN32
  Options opts;
  opts.pipeStdout();
  opts.pipeStdin();
  ChildProcess cat({"/bin/cat", "-"}, std::move(opts));

  std::vector<std::string> expected{"one", "two", "three"};
  std::list<std::string> lines{"one\n", "two\n", "three\n"};

  auto writable = [&lines](watchman::FileDescriptor& fd) {
    if (lines.empty()) {
      return true;
    }
    auto str = lines.front();
    if (write(fd.fd(), str.data(), str.size()) == -1) {
      throw std::runtime_error("write to child failed");
    }
    lines.pop_front();
    return false;
  };

  auto outputs =
      threaded ? cat.threadedCommunicate(writable) : cat.communicate(writable);
  cat.wait();

  std::vector<std::string> resultLines;
  w_string_piece(outputs.first).split(resultLines, '\n');
  ok(resultLines.size() == 3, "got three lines of output");
  ok(resultLines == expected, "got all input lines");
#else
  (void)threaded;
  pass("dummy1");
  pass("dummy2");
#endif
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(5);
  test_pipe();
  test_pipe_input(true);
  test_pipe_input(false);

  return exit_status();
}
