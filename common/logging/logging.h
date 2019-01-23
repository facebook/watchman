/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

// TODO: actually implement this
#ifndef VLOG_EVERY_MS
#define VLOG_EVERY_MS(verboselevel, ms)     \
  VLOG(verboselevel)
#endif
