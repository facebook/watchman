/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "QueryableView.h"
#include "watchman/Errors.h"

namespace watchman {
QueryableView::~QueryableView() {}

/** Perform a time-based (since) query and emit results to the supplied
 * query context */
void QueryableView::timeGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("timeGenerator not implemented");
}

/** Walks files that match the supplied set of paths */
void QueryableView::pathGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("pathGenerator not implemented");
}

void QueryableView::globGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("globGenerator not implemented");
}

void QueryableView::allFilesGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("allFilesGenerator not implemented");
}

uint32_t QueryableView::getLastAgeOutTickValue() const {
  return 0;
}

std::chrono::system_clock::time_point QueryableView::getLastAgeOutTimeStamp()
    const {
  return std::chrono::system_clock::time_point{};
}

void QueryableView::ageOut(w_perf_t&, std::chrono::seconds) {}

bool QueryableView::isVCSOperationInProgress() const {
  static const std::vector<w_string> lockFiles{".hg/wlock", ".git/index.lock"};
  return doAnyOfTheseFilesExist(lockFiles);
}
} // namespace watchman
