/*
 * Copyright 2017-present Facebook. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

package com.facebook.watchman.windowspipe;

import com.sun.jna.Memory;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.platform.win32.WinBase;
import com.sun.jna.platform.win32.WinNT;
import com.sun.jna.ptr.IntByReference;
import com.sun.jna.win32.W32APIOptions;

public interface WindowsNamedPipeLibrary extends WinNT {
  WindowsNamedPipeLibrary INSTANCE = (WindowsNamedPipeLibrary) Native.loadLibrary("kernel32",
      WindowsNamedPipeLibrary.class, W32APIOptions.UNICODE_OPTIONS);

  boolean GetOverlappedResult(HANDLE hFile,
                              Pointer lpOverlapped,
                              IntByReference lpNumberOfBytesTransferred,
                              boolean wait);

  boolean ReadFile(HANDLE hFile, Memory pointer, int nNumberOfBytesToRead,
                   IntByReference lpNumberOfBytesRead, Pointer lpOverlapped);

  HANDLE CreateFile(String lpFileName, int dwDesiredAccess, int dwShareMode,
                    WinBase.SECURITY_ATTRIBUTES lpSecurityAttributes,
                    int dwCreationDisposition, int dwFlagsAndAttributes,
                    HANDLE hTemplateFile);

  HANDLE CreateEvent(WinBase.SECURITY_ATTRIBUTES lpEventAttributes,
                     boolean bManualReset, boolean bInitialState, String lpName);

  boolean CloseHandle(HANDLE hObject);

  boolean WriteFile(HANDLE hFile, byte[] lpBuffer, int nNumberOfBytesToWrite,
                    IntByReference lpNumberOfBytesWritten,
                    Pointer lpOverlapped);

  int GetLastError();
}
