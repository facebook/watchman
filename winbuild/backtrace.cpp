/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <Dbghelp.h>

static pthread_once_t sym_init_once;
static HANDLE proc;

static void sym_init(void) {
  proc = GetCurrentProcess();
  SymInitialize(proc, NULL, TRUE);
  SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_FAIL_CRITICAL_ERRORS|
      SYMOPT_NO_PROMPTS|SYMOPT_UNDNAME);
}

size_t backtrace(void **frames, size_t n_frames) {
  // Skip the first three frames; they're always going to show
  // w_log, log_stack_trace and backtrace
  return CaptureStackBackTrace(3, (DWORD)n_frames, frames, NULL);
}

char **backtrace_symbols(void **array, size_t n_frames) {
  auto arr = json_array_of_size(n_frames);
  char **strings;
  size_t i;
  union {
    SYMBOL_INFO info;
    char buf[1024];
  } sym;
  IMAGEHLP_LINE64 line;

  pthread_once(&sym_init_once, sym_init);

  memset(&sym.info, 0, sizeof(sym.info));
  sym.info.MaxNameLen = sizeof(sym) - sizeof(sym.info);
  sym.info.SizeOfStruct = sizeof(sym.info);

  line.SizeOfStruct = sizeof(line);

  for (i = 0; i < n_frames; i++) {
    char str[1024];
    DWORD64 addr = (DWORD64)(intptr_t)array[i];
    DWORD displacement;

    SymFromAddr(proc, addr, 0, &sym.info);

    if (SymGetLineFromAddr64(proc, addr, &displacement, &line)) {
      snprintf(str, sizeof(str), "#%" PRIsize_t " %p %s %s:%u", i,
        array[i], sym.info.Name, line.FileName, line.LineNumber);

    } else {
      snprintf(str, sizeof(str), "#%" PRIsize_t " %p %s", i,
        array[i], sym.info.Name);
    }

    json_array_append_new(arr, typed_string_to_json(str, W_STRING_MIXED));
  }

  strings = w_argv_copy_from_json(arr, 0);
  return strings;
}
