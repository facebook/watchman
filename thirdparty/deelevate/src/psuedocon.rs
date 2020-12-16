use crate::pipe::PipeHandle;
use lazy_static::lazy_static;
use shared_library::shared_library;
use std::io::{Error as IoError, Result as IoResult};
use std::os::windows::io::AsRawHandle;
use std::path::Path;
use winapi::shared::minwindef::DWORD;
use winapi::shared::winerror::{HRESULT, S_OK};
use winapi::um::handleapi::*;
use winapi::um::wincon::COORD;
use winapi::um::winnt::HANDLE;

pub type HPCON = HANDLE;

shared_library!(ConPtyFuncs,
    pub fn CreatePseudoConsole(
        size: COORD,
        hInput: HANDLE,
        hOutput: HANDLE,
        flags: DWORD,
        hpc: *mut HPCON
    ) -> HRESULT,
    pub fn ResizePseudoConsole(hpc: HPCON, size: COORD) -> HRESULT,
    pub fn ClosePseudoConsole(hpc: HPCON),
);

fn load_conpty() -> ConPtyFuncs {
    // If the kernel doesn't export these functions then their system is
    // too old and we cannot run.
    let kernel = ConPtyFuncs::open(Path::new("kernel32.dll")).expect(
        "this system does not support conpty.  Windows 10 October 2018 or newer is required",
    );

    // We prefer to use a sideloaded conpty.dll and openconsole.exe host deployed
    // alongside the application.  We check for this after checking for kernel
    // support so that we don't try to proceed and do something crazy.
    if let Ok(sideloaded) = ConPtyFuncs::open(Path::new("conpty.dll")) {
        sideloaded
    } else {
        kernel
    }
}

lazy_static! {
    static ref CONPTY: ConPtyFuncs = load_conpty();
}

pub struct PsuedoCon {
    pub(crate) con: HPCON,
}

unsafe impl Send for PsuedoCon {}
unsafe impl Sync for PsuedoCon {}

impl Drop for PsuedoCon {
    fn drop(&mut self) {
        unsafe { (CONPTY.ClosePseudoConsole)(self.con) };
    }
}

impl PsuedoCon {
    pub fn new(size: COORD, input: PipeHandle, output: PipeHandle) -> IoResult<Self> {
        let mut con: HPCON = INVALID_HANDLE_VALUE;
        let result = unsafe {
            (CONPTY.CreatePseudoConsole)(
                size,
                input.as_raw_handle() as _,
                output.as_raw_handle() as _,
                0,
                &mut con,
            )
        };
        if result != S_OK {
            Err(IoError::new(
                std::io::ErrorKind::Other,
                format!("failed to create psuedo console: HRESULT {}", result),
            ))
        } else {
            Ok(Self { con })
        }
    }

    pub fn resize(&self, size: COORD) -> IoResult<()> {
        let result = unsafe { (CONPTY.ResizePseudoConsole)(self.con, size) };
        if result != S_OK {
            Err(IoError::new(
                std::io::ErrorKind::Other,
                format!(
                    "failed to resize console to {}x{}: HRESULT: {}",
                    size.X, size.Y, result
                ),
            ))
        } else {
            Ok(())
        }
    }
}
