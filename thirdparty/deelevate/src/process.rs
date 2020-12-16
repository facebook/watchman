//! Working with process handles
use crate::win32_error_with_context;
use std::io::{Error as IoError, Result as IoResult};
use winapi::shared::minwindef::DWORD;
use winapi::um::handleapi::{CloseHandle, INVALID_HANDLE_VALUE};
use winapi::um::processthreadsapi::{GetExitCodeProcess, OpenProcess};
use winapi::um::synchapi::WaitForSingleObject;
use winapi::um::winbase::{INFINITE, WAIT_FAILED};
use winapi::um::winnt::HANDLE;

/// An owning wrapper around handles that represent processes
pub struct Process(HANDLE);
/// The compiler thinks it isn't send because HANDLE is a pointer
/// type.  We happen to know that moving the handle between threads
/// is totally fine, hence this impl.
unsafe impl Send for Process {}

impl Drop for Process {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

impl Process {
    /// Delegates to the OpenProcess win32 API
    pub fn with_process_id(
        desired_access: DWORD,
        inherit_handles: bool,
        pid: DWORD,
    ) -> IoResult<Self> {
        let proc = unsafe { OpenProcess(desired_access, inherit_handles as _, pid) };
        if proc == INVALID_HANDLE_VALUE {
            Err(win32_error_with_context(
                "OpenProcess",
                IoError::last_os_error(),
            ))
        } else {
            Ok(Self(proc))
        }
    }

    /// Returns the underlying raw handle value
    pub fn as_handle(&self) -> HANDLE {
        self.0
    }

    /// Takes ownership of the provided handle and will close
    /// it when this Process instance is dropped!
    pub fn with_handle(proc: HANDLE) -> Self {
        Self(proc)
    }

    /// Wait for the specified duration (in milliseconds!) to pass.
    /// Use None to wait forever.
    pub fn wait_for(&self, duration: Option<DWORD>) -> IoResult<DWORD> {
        let res = unsafe { WaitForSingleObject(self.0, duration.unwrap_or(INFINITE)) };
        if res == WAIT_FAILED {
            Err(win32_error_with_context(
                "WaitForSingleObject(process)",
                IoError::last_os_error(),
            ))
        } else {
            Ok(res)
        }
    }

    /// Retrieves the exit code from the process
    pub fn exit_code(&self) -> IoResult<DWORD> {
        let mut exit_code = 0;
        if unsafe { GetExitCodeProcess(self.0, &mut exit_code) } != 0 {
            Ok(exit_code)
        } else {
            Err(win32_error_with_context(
                "GetExitCodeProcess",
                IoError::last_os_error(),
            ))
        }
    }
}
