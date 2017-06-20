/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "QueryableView.h"

namespace watchman {
QueryableView::~QueryableView() {}

/** Perform a time-based (since) query and emit results to the supplied
   * query context */
void QueryableView::timeGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("timeGenerator not implemented");
}

/** Walks all files with the suffix(es) configured in the query */
void QueryableView::suffixGenerator(w_query*, struct w_query_ctx*) const {
  throw QueryExecError("suffixGenerator not implemented");
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

time_t QueryableView::getLastAgeOutTimeStamp() const {
  return 0;
}

void QueryableView::ageOut(w_perf_t&, std::chrono::seconds) {}
void QueryableView::startThreads(const std::shared_ptr<w_root_t>&) {}
void QueryableView::signalThreads() {}
void QueryableView::wakeThreads() {}

bool QueryableView::isVCSOperationInProgress() const {
  static const std::vector<w_string> lockFiles{".hg/wlock", ".git/index.lock"};
  return doAnyOfTheseFilesExist(lockFiles);
}
}
