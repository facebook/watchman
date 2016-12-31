/* Copyright 2014-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
// some versions of dbghelp.h do: typedef enum {}; with no typedef name
#pragma warning(disable: 4091)
#include <Dbghelp.h>
#include <mutex>

static std::once_flag sym_init_once;
static HANDLE proc;
// 4k for a symbol name? Demangled symbols are pretty huge
static constexpr size_t kMaxSymbolLen = 4096;

static void sym_init(void) {
  proc = GetCurrentProcess();
  SymInitialize(proc, NULL, TRUE);
  SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_FAIL_CRITICAL_ERRORS|
      SYMOPT_NO_PROMPTS|SYMOPT_UNDNAME);
}

size_t backtrace(void **frames, size_t n_frames) {
  std::call_once(sym_init_once, sym_init);
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
    char buf[kMaxSymbolLen];
  } sym;
  IMAGEHLP_LINE64 line;

  std::call_once(sym_init_once, sym_init);

  memset(&sym.info, 0, sizeof(sym.info));
  sym.info.MaxNameLen = sizeof(sym) - sizeof(sym.info);
  sym.info.SizeOfStruct = sizeof(sym.info);

  line.SizeOfStruct = sizeof(line);

  for (i = 0; i < n_frames; i++) {
    char str[kMaxSymbolLen + 128];
    DWORD64 addr = (DWORD64)(intptr_t)array[i];
    DWORD displacement;

    if (!SymFromAddr(proc, addr, 0, &sym.info)) {
      snprintf(
          sym.info.Name,
          sizeof(sym.buf),
          "<failed to resolve symbol: %s>",
          std::system_category().message(GetLastError()).c_str());
    }

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

size_t backtrace_from_exception(
    LPEXCEPTION_POINTERS exception,
    void** frames,
    size_t n_frames) {
  std::call_once(sym_init_once, sym_init);

  auto context = exception->ContextRecord;
  auto thread = GetCurrentThread();
  STACKFRAME64 frame;
  DWORD image;
  size_t i = 0;
#ifdef _M_IX86
  image = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = context->Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context->Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context->Esp;
  frame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
  image = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = context->Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context->Rsp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context->Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
  image = IMAGE_FILE_MACHINE_IA64;
  frame.AddrPC.Offset = context->StIIP;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context->IntSp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrBStore.Offset = context->RsBSP;
  frame.AddrBStore.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context->IntSp;
  frame.AddrStack.Mode = AddrModeFlat;
#else
  return 0; // No stack trace for you!
#endif
  while (i < n_frames && StackWalk64(
                             image,
                             proc,
                             thread,
                             &frame,
                             context,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr)) {
    frames[i++] = (void*)frame.AddrPC.Offset;
  }
  return i;
}
