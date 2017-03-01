/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "Mercurial.h"
#include "ChildProcess.h"
#include "watchman.h"

// Capability indicating support for the mercurial SCM
W_CAP_REG("scm-hg")

namespace watchman {

using Options = ChildProcess::Options;

Mercurial::Mercurial(w_string_piece rootPath, w_string_piece scmRoot)
    : SCM(rootPath, scmRoot) {}

w_string Mercurial::mergeBaseWith(w_string_piece commitId) const {
  Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", "1");
  opt.pipeStdout();
  opt.chdir(getRootPath());

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {"hg", "log", "-T", "{node}", "-r", revset}, std::move(opt));

  proc.pipe(STDOUT_FILENO).read.clearNonBlock();
  auto status = proc.wait();

  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the merge base; command returned with status ",
        status));
  }

  char buf[128];
  auto len = read(proc.pipe(STDOUT_FILENO).read.fd(), buf, sizeof(buf));

  if (len != 40) {
    throw std::runtime_error(to<std::string>(
        "expected merge base to be a 40 character string, got ",
        w_string_piece(buf, len)));
  }

  return w_string(buf, len);
}

std::vector<w_string> Mercurial::getFilesChangedSinceMergeBaseWith(
    w_string_piece commitId) const {
  Options opt;
  // Ensure that the hgrc doesn't mess with the behavior
  // of the commands that we're runing.
  opt.environment().set("HGPLAIN", "1");
  opt.pipeStdout();
  opt.chdir(getRootPath());

  auto revset = to<std::string>("ancestor(.,", commitId, ")");
  ChildProcess proc(
      {"hg", "status", "-n", "--root-relative", "--rev", commitId},
      std::move(opt));

  proc.pipe(STDOUT_FILENO).read.clearNonBlock();
  std::vector<w_string> res;

  std::string line;
  char buf[512];

  while (true) {
    auto len = read(proc.pipe(STDOUT_FILENO).read.fd(), buf, sizeof(buf));
    if (len == 0) {
      break;
    }
    if (len < 0) {
      throw std::system_error(
          errno,
          std::generic_category(),
          "error while reading from hg status pipe");
    }
    auto shortRead = len < int(sizeof(buf));

    line.append(buf, len);

    // Note: mercurial always ends the last line with a newline,
    // so we only need do line splitting here.
    size_t start = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\n') {
        len = i - start;
        if (len > 0) {
          res.emplace_back(&line[start], len);
        }
        start = i + 1;
      }
    }

    if (start > 0) {
      line.erase(0, start);
    }

    if (shortRead) {
      // A short read means that we got to the end of the pipe data
      break;
    }
  }

  w_check(line.empty(), "we should have consumed all the data");

  auto status = proc.wait();
  if (status) {
    throw std::runtime_error(to<std::string>(
        "failed query for the hg status; command returned with status ",
        status));
  }

  return res;
}
}
