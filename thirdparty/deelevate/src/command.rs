use crate::pipe::*;
use crate::process::Process;
use crate::procthreadattr::ProcThreadAttributeList;
use crate::psuedocon::PsuedoCon;
use crate::{os_str_to_null_terminated_vec, win32_error_with_context, Token};
use std::ffi::{OsStr, OsString};
use std::io::{Error as IoError, Result as IoResult};
use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;
use std::ptr::null_mut;
use winapi::shared::minwindef::{BOOL, DWORD, LPVOID};
use winapi::um::combaseapi::CoInitializeEx;
use winapi::um::handleapi::{CloseHandle, INVALID_HANDLE_VALUE};
use winapi::um::objbase::{COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE};
use winapi::um::processthreadsapi::{
    CreateProcessAsUserW, CreateProcessW, PROCESS_INFORMATION, STARTUPINFOW,
};
use winapi::um::shellapi::{ShellExecuteExW, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW};
use winapi::um::userenv::{CreateEnvironmentBlock, DestroyEnvironmentBlock};
use winapi::um::winbase::{
    CREATE_DEFAULT_ERROR_MODE, CREATE_NEW_CONSOLE, CREATE_NEW_PROCESS_GROUP,
    CREATE_UNICODE_ENVIRONMENT, EXTENDED_STARTUPINFO_PRESENT, STARTF_USESHOWWINDOW,
    STARTF_USESTDHANDLES, STARTUPINFOEXW,
};
use winapi::um::winnt::{HANDLE, LPCWSTR, LPWSTR};
use winapi::um::winuser::{SW_HIDE, SW_SHOWNORMAL};

extern "system" {
    /// This is missing from the currently available versions of the winapi crate.
    fn CreateProcessWithTokenW(
        hToken: HANDLE,
        dwLogonFlags: DWORD,
        lpApplicationName: LPCWSTR,
        lpCommandLine: LPWSTR,
        dwCreationFlags: DWORD,
        lpEnvironment: LPVOID,
        lpCurrentDirectory: LPCWSTR,
        lpStartupInfo: *mut STARTUPINFOW,
        lpProcessInformation: *mut PROCESS_INFORMATION,
    ) -> BOOL;
}

pub struct EnvironmentBlock(pub LPVOID);
impl Drop for EnvironmentBlock {
    fn drop(&mut self) {
        unsafe {
            DestroyEnvironmentBlock(self.0);
        }
    }
}

impl EnvironmentBlock {
    /// Create a copy of the current environment, but do so for the provided token.
    /// We have to do this explicitly as some of the CreateProcessAsXXX
    /// calls will default to a different process environment otherwise!
    pub fn with_token(token: &Token) -> IoResult<Self> {
        let mut block = null_mut();
        let inherit = true;
        if unsafe { CreateEnvironmentBlock(&mut block, token.token, inherit as _) } != 1 {
            Err(win32_error_with_context(
                "CreateEnvironmentBlock",
                IoError::last_os_error(),
            ))
        } else {
            Ok(Self(block))
        }
    }

    pub fn as_vec(&self) -> Vec<u16> {
        // This is safe because we know that the block was created by
        // CreateEnvironmentBlock and that it is well formed.
        // An environment block has the form:
        // key=value\0
        // ...
        // key=value\0
        // \0
        // So when we find the sequence \0\0 then we have found the extent
        // of the block.
        unsafe {
            let mut ptr = self.0 as *const u16;
            let mut size = 0;
            loop {
                let next = ptr.add(1);
                if ptr.read() == 0 {
                    if next.read() == 0 {
                        // We found the double-null terminator
                        size += 2;
                        break;
                    }
                }
                ptr = next;
                size += 1;
            }

            let slice = std::slice::from_raw_parts(self.0 as *const u16, size);
            slice.to_vec()
        }
    }
}

/// Helper for ensuring that handles from a spawned
/// process are closed
struct ProcInfo(PROCESS_INFORMATION);
impl Drop for ProcInfo {
    fn drop(&mut self) {
        unsafe {
            if !self.0.hProcess.is_null() {
                CloseHandle(self.0.hProcess);
            }
            if !self.0.hThread.is_null() {
                CloseHandle(self.0.hThread);
            }
        }
    }
}

impl ProcInfo {
    pub fn new() -> Self {
        Self(unsafe { std::mem::zeroed() })
    }

    /// Take ownership of the process handle
    pub fn process(&mut self) -> Option<Process> {
        if self.0.hProcess.is_null() {
            None
        } else {
            let proc = Process::with_handle(self.0.hProcess);
            self.0.hProcess = null_mut();
            Some(proc)
        }
    }
}

pub struct Command {
    args: Vec<OsString>,
    env: Vec<u16>,
    cwd: PathBuf,
    hide_window: bool,
    stdin: Option<PipeHandle>,
    stdout: Option<PipeHandle>,
    stderr: Option<PipeHandle>,
}

impl Command {
    pub fn with_environment_for_token(token: &Token) -> IoResult<Self> {
        let env = EnvironmentBlock::with_token(token)?.as_vec();
        let cwd = std::env::current_dir()?;
        Ok(Self {
            args: vec![],
            env,
            cwd,
            stdin: None,
            stdout: None,
            stderr: None,
            hide_window: false,
        })
    }

    pub fn set_command_from_current_process(&mut self) -> IoResult<()> {
        self.args = std::env::args_os().collect();
        Ok(())
    }

    pub fn hide_window(&mut self) {
        self.hide_window = true;
    }

    pub fn set_argv(&mut self, argv: Vec<OsString>) {
        self.args = argv;
    }

    fn executable_and_command_line(&self, skip: usize) -> (Vec<u16>, Vec<u16>) {
        let executable = os_str_to_null_terminated_vec(&self.args[0]);

        let mut cmdline = Vec::<u16>::new();
        for arg in self.args.iter().skip(skip) {
            if !cmdline.is_empty() {
                cmdline.push(' ' as u16);
            }
            append_quoted(&arg, &mut cmdline);
        }
        cmdline.push(0);

        (executable, cmdline)
    }

    pub fn set_stdin(&mut self, p: PipeHandle) -> IoResult<()> {
        p.make_inheritable()?;
        self.stdin.replace(p);
        Ok(())
    }

    pub fn set_stdout(&mut self, p: PipeHandle) -> IoResult<()> {
        p.make_inheritable()?;
        self.stdout.replace(p);
        Ok(())
    }

    pub fn set_stderr(&mut self, p: PipeHandle) -> IoResult<()> {
        p.make_inheritable()?;
        self.stderr.replace(p);
        Ok(())
    }

    fn make_startup_info(&self) -> STARTUPINFOW {
        let mut si: STARTUPINFOW = unsafe { std::mem::zeroed() };
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        si.dwFlags = 0;

        if self.hide_window {
            si.dwFlags |= STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE as _;
        }

        if self.stdin.is_some() || self.stdout.is_some() || self.stderr.is_some() {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = INVALID_HANDLE_VALUE;
            si.hStdOutput = INVALID_HANDLE_VALUE;
            si.hStdError = INVALID_HANDLE_VALUE;
        }

        if let Some(pipe) = self.stdin.as_ref() {
            si.hStdInput = pipe.as_handle();
        }
        if let Some(pipe) = self.stdout.as_ref() {
            si.hStdOutput = pipe.as_handle();
        }
        if let Some(pipe) = self.stderr.as_ref() {
            si.hStdError = pipe.as_handle();
        }

        si
    }

    pub fn shell_execute(&mut self, verb: &str) -> IoResult<Process> {
        unsafe {
            CoInitializeEx(
                null_mut(),
                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE,
            )
        };
        let (exe, params) = self.executable_and_command_line(1);
        let cwd = os_str_to_null_terminated_vec(self.cwd.as_os_str());
        let verb = os_str_to_null_terminated_vec(OsStr::new(verb));

        let mut info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_NOCLOSEPROCESS,
            hwnd: null_mut(),
            lpVerb: verb.as_ptr(),
            lpFile: exe.as_ptr(),
            lpParameters: params.as_ptr(),
            lpDirectory: cwd.as_ptr(),
            nShow: if self.hide_window {
                SW_HIDE
            } else {
                SW_SHOWNORMAL
            },
            hInstApp: null_mut(),
            lpIDList: null_mut(),
            lpClass: null_mut(),
            hkeyClass: null_mut(),
            hMonitor: null_mut(),
            dwHotKey: 0,
            hProcess: null_mut(),
        };

        let res = unsafe { ShellExecuteExW(&mut info) };

        if res == 0 {
            Err(win32_error_with_context(
                "ShellExecuteExW",
                IoError::last_os_error(),
            ))
        } else {
            Ok(Process::with_handle(info.hProcess))
        }
    }

    pub fn spawn_with_pty(&mut self, psuedocon: &PsuedoCon) -> IoResult<Process> {
        let mut si = STARTUPINFOEXW {
            StartupInfo: self.make_startup_info(),
            lpAttributeList: null_mut(),
        };
        si.StartupInfo.cb = std::mem::size_of::<STARTUPINFOEXW>() as u32;

        let mut attrs = ProcThreadAttributeList::with_capacity(1)?;
        attrs.set_pty(psuedocon.con)?;
        si.lpAttributeList = attrs.as_mut_ptr();

        let mut pi = ProcInfo::new();
        let (mut exe, mut command_line) = self.executable_and_command_line(0);
        let mut cwd = os_str_to_null_terminated_vec(self.cwd.as_os_str());

        let proc_attributes = null_mut();
        let thread_attributes = null_mut();
        let inherit_handles = true;

        let res = unsafe {
            CreateProcessW(
                exe.as_mut_ptr(),
                command_line.as_mut_ptr(),
                proc_attributes,
                thread_attributes,
                inherit_handles as _,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                self.env.as_mut_ptr() as *mut _,
                cwd.as_mut_ptr(),
                &mut si.StartupInfo,
                &mut pi.0,
            )
        };
        if res == 0 {
            Err(win32_error_with_context(
                "CreateProcessAsUserW",
                IoError::last_os_error(),
            ))
        } else {
            Ok(pi.process().unwrap())
        }
    }

    pub fn spawn(&mut self) -> IoResult<Process> {
        let mut si = self.make_startup_info();
        let mut pi = ProcInfo::new();
        let (mut exe, mut command_line) = self.executable_and_command_line(0);
        let mut cwd = os_str_to_null_terminated_vec(self.cwd.as_os_str());

        let proc_attributes = null_mut();
        let thread_attributes = null_mut();
        let inherit_handles = true;

        let res = unsafe {
            CreateProcessW(
                exe.as_mut_ptr(),
                command_line.as_mut_ptr(),
                proc_attributes,
                thread_attributes,
                inherit_handles as _,
                CREATE_UNICODE_ENVIRONMENT,
                self.env.as_mut_ptr() as *mut _,
                cwd.as_mut_ptr(),
                &mut si,
                &mut pi.0,
            )
        };
        if res != 1 {
            Err(win32_error_with_context(
                "CreateProcessAsUserW",
                IoError::last_os_error(),
            ))
        } else {
            Ok(pi.process().unwrap())
        }
    }

    pub fn spawn_as_user(&mut self, token: &Token) -> IoResult<Process> {
        let mut si = self.make_startup_info();
        let mut pi = ProcInfo::new();
        let (mut exe, mut command_line) = self.executable_and_command_line(0);
        let mut cwd = os_str_to_null_terminated_vec(self.cwd.as_os_str());

        let proc_attributes = null_mut();
        let thread_attributes = null_mut();
        let inherit_handles = true;

        let res = unsafe {
            CreateProcessAsUserW(
                token.token,
                exe.as_mut_ptr(),
                command_line.as_mut_ptr(),
                proc_attributes,
                thread_attributes,
                inherit_handles as _,
                CREATE_UNICODE_ENVIRONMENT,
                self.env.as_mut_ptr() as *mut _,
                cwd.as_mut_ptr(),
                &mut si,
                &mut pi.0,
            )
        };
        if res != 1 {
            Err(win32_error_with_context(
                "CreateProcessAsUserW",
                IoError::last_os_error(),
            ))
        } else {
            Ok(pi.process().unwrap())
        }
    }

    pub fn spawn_with_token(&mut self, token: &Token) -> IoResult<Process> {
        let mut si = self.make_startup_info();

        let mut pi = ProcInfo::new();
        let (mut exe, mut command_line) = self.executable_and_command_line(0);
        let mut cwd = os_str_to_null_terminated_vec(self.cwd.as_os_str());

        let logon_flags = 0;

        let res = unsafe {
            CreateProcessWithTokenW(
                token.token,
                logon_flags,
                exe.as_mut_ptr(),
                command_line.as_mut_ptr(),
                CREATE_UNICODE_ENVIRONMENT|
                // Note that these flags are unconditionally or'd
                // in by CreateProcessWithTokenW: they're included
                // here to make it more obvious that these apply:
                CREATE_DEFAULT_ERROR_MODE|
                CREATE_NEW_CONSOLE|
                CREATE_NEW_PROCESS_GROUP,
                self.env.as_mut_ptr() as *mut _,
                cwd.as_mut_ptr(),
                &mut si,
                &mut pi.0,
            )
        };
        if res != 1 {
            Err(win32_error_with_context(
                "CreateProcessWithTokenW",
                IoError::last_os_error(),
            ))
        } else {
            Ok(pi.process().unwrap())
        }
    }
}

// Borrowed from https://github.com/wez/wezterm/blob/65707aba56f940f8c370f0465f0f3f2a6303a9cc/pty/src/cmdbuilder.rs#L313
// and thus from https://github.com/hniksic/rust-subprocess/blob/873dfed165173e52907beb87118b2c0c05d8b8a1/src/popen.rs#L1117
// which in turn was translated from ArgvQuote at http://tinyurl.com/zmgtnls
fn append_quoted(arg: &OsStr, cmdline: &mut Vec<u16>) {
    if !arg.is_empty()
        && !arg.encode_wide().any(|c| {
            c == ' ' as u16
                || c == '\t' as u16
                || c == '\n' as u16
                || c == '\x0b' as u16
                || c == '\"' as u16
        })
    {
        cmdline.extend(arg.encode_wide());
        return;
    }
    cmdline.push('"' as u16);

    let arg: Vec<_> = arg.encode_wide().collect();
    let mut i = 0;
    while i < arg.len() {
        let mut num_backslashes = 0;
        while i < arg.len() && arg[i] == '\\' as u16 {
            i += 1;
            num_backslashes += 1;
        }

        if i == arg.len() {
            for _ in 0..num_backslashes * 2 {
                cmdline.push('\\' as u16);
            }
            break;
        } else if arg[i] == b'"' as u16 {
            for _ in 0..num_backslashes * 2 + 1 {
                cmdline.push('\\' as u16);
            }
            cmdline.push(arg[i]);
        } else {
            for _ in 0..num_backslashes {
                cmdline.push('\\' as u16);
            }
            cmdline.push(arg[i]);
        }
        i += 1;
    }
    cmdline.push('"' as u16);
}
