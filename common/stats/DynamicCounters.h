// Copyright 2004-present Facebook. All Rights Reserved.
#pragma once

#include <folly/Range.h>
#include <functional>

namespace facebook {
namespace stats {

class DynamicCounters {
 public:
  using Callback = std::function<void()>;

  void registerCallback(folly::StringPiece /* name */,
                        const Callback & /* callback */) {
  }
  void unregisterCallback(folly::StringPiece /* name */) {
  }
};

}
}
