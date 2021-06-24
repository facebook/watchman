/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */
#include <folly/portability/GTest.h>
#include <list>
#include "watchman/ChildProcess.h"
#include "watchman/watchman_system.h"

using watchman::ChildProcess;
using Options = ChildProcess::Options;

TEST(ChildProcess, pipe) {
  Options opts;
  opts.pipeStdout();
  ChildProcess echo(
      {
#ifndef _WIN32
          "echo",
#else
          // If we're being built via cmake we know that we
          // have the cmake executable on hand to invoke its
          // echo program
          "cmake",
          "-E",
          "echo",
#endif
          "hello"},
      std::move(opts));

  auto outputs = echo.communicate();
  echo.wait();

  w_string_piece line(outputs.first);
  EXPECT_TRUE(line.startsWith("hello"));
}

void test_pipe_input(bool threaded) {
#ifndef _WIN32
  Options opts;
  opts.pipeStdout();
  opts.pipeStdin();
  ChildProcess cat({"cat", "-"}, std::move(opts));

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
  EXPECT_EQ(resultLines.size(), 3);
  EXPECT_EQ(resultLines, expected);
#else
  (void)threaded;
#endif
}

TEST(ChildProcess, stresstest_pipe_output) {
  bool okay = true;
#ifndef _WIN32
  for (int i = 0; i < 3000; ++i) {
    Options opts;
    opts.pipeStdout();
    ChildProcess proc({"head", "-n20", "/dev/urandom"}, std::move(opts));
    auto outputs = proc.communicate();
    w_string_piece out(outputs.first);
    proc.wait();
    if (out.empty() || out[out.size() - 1] != '\n') {
      okay = false;
      break;
    }
  }
#endif
  EXPECT_TRUE(okay);
}

TEST(ChildProcess, inputThreaded) {
  test_pipe_input(true);
}

TEST(ChildProcess, inputNotThreaded) {
  test_pipe_input(false);
}
