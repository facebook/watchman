/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "SCM.h"
#include <memory>
#include "Mercurial.h"
#include "watchman/watchman.h"

namespace watchman {

static const w_string_piece kGit{".git"};
static const w_string_piece kHg{".hg"};

SCM::~SCM() {}

SCM::SCM(w_string_piece rootPath, w_string_piece scmRoot)
    : rootPath_(rootPath.asWString()), scmRoot_(scmRoot.asWString()) {}

const w_string& SCM::getRootPath() const {
  return rootPath_;
}

const w_string& SCM::getSCMRoot() const {
  return scmRoot_;
}

w_string findFileInDirTree(
    w_string_piece rootPath,
    std::initializer_list<w_string_piece> candidates) {
  w_check(rootPath.pathIsAbsolute(), "rootPath must be absolute");

  while (true) {
    for (auto& candidate : candidates) {
      auto path = w_string::pathCat({rootPath, candidate});

      if (w_path_exists(path.c_str())) {
        return path;
      }
    }

    auto next = rootPath.dirName();
    if (next == rootPath) {
      // We can't go any higher, so we couldn't find the
      // requested path(s)
      return nullptr;
    }

    rootPath = next;
  }
}

std::unique_ptr<SCM> SCM::scmForPath(w_string_piece rootPath) {
  auto scmRoot = findFileInDirTree(rootPath, {kHg, kGit});

  if (!scmRoot) {
    return nullptr;
  }

  auto base = scmRoot.piece().baseName();

  if (base == kHg) {
    return std::make_unique<Mercurial>(rootPath, scmRoot.piece().dirName());
  }

  if (base == kGit) {
  }

  return nullptr;
}
} // namespace watchman
