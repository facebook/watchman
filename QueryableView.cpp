/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

#include "QueryableView.h"

namespace watchman {
QueryableView::~QueryableView() {}

/** Perform a time-based (since) query and emit results to the supplied
   * query context */
bool QueryableView::timeGenerator(w_query*, struct w_query_ctx*, int64_t*)
    const {
  return false;
}

/** Walks all files with the suffix(es) configured in the query */
bool QueryableView::suffixGenerator(w_query*, struct w_query_ctx*, int64_t*)
    const {
  return false;
}

/** Walks files that match the supplied set of paths */
bool QueryableView::pathGenerator(w_query*, struct w_query_ctx*, int64_t*)
    const {
  return false;
}

bool QueryableView::globGenerator(w_query*, struct w_query_ctx*, int64_t*)
    const {
  return false;
}

bool QueryableView::allFilesGenerator(w_query*, struct w_query_ctx*, int64_t*)
    const {
  return false;
}

uint32_t QueryableView::getMostRecentTickValue() const {
  return 0;
}

uint32_t QueryableView::getLastAgeOutTickValue() const {
  return 0;
}

time_t QueryableView::getLastAgeOutTimeStamp() const {
  return 0;
}

void QueryableView::ageOut(w_perf_t&, std::chrono::seconds) {}
void QueryableView::startThreads(w_root_t*) {}
void QueryableView::signalThreads() {}

bool QueryableView::isVCSOperationInProgress() const {
  static const std::vector<w_string> lockFiles{".hg/wlock", ".git/index.lock"};
  return doAnyOfTheseFilesExist(lockFiles);
}
}
