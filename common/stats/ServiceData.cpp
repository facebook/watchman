/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "common/stats/ServiceData.h"

static facebook::stats::ServiceData payload;

namespace facebook {
namespace stats {

ServiceData* ServiceData::get() {
  return &payload;
}
} // namespace stats

facebook::stats::ServiceData* fbData = &payload;
} // namespace facebook
