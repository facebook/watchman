/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"

int gethostname(char *buf, int bufsize) {
  DWORD size = bufsize;

  if (GetComputerNameEx(ComputerNamePhysicalDnsHostname, buf, &size)) {
    return 0;
  }

  errno = map_win32_err(GetLastError());
  return -1;
}
