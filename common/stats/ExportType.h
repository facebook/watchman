// Copyright 2004-present Facebook. All Rights Reserved.
#pragma once

namespace facebook {
namespace stats {

enum ExportType : int {
  SUM,
  COUNT,
  AVG,
  RATE,
  PERCENT,
};

}
}
