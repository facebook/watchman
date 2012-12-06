/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"

void w_log(int level, const char *fmt, ...)
{
  char buf[4096];
  va_list ap;
  int len;

  len = snprintf(buf, sizeof(buf), "%d: ", (int)time(NULL));
  va_start(ap, fmt);
  len += vsnprintf(buf + len, sizeof(buf), fmt, ap);
  va_end(ap);

  ignore_result(write(STDERR_FILENO, buf, len));

  w_log_to_clients(level, buf);
}

/* vim:ts=2:sw=2:et:
 */

