use std::ffi::OsStr;
use std::io::Error as IoError;
use std::os::windows::ffi::OsStrExt;

mod bridge;
mod command;
mod pipe;
mod process;
mod procthreadattr;
mod psuedocon;
mod sid;
mod spawn;
mod token;

pub use bridge::{BridgePtyClient, BridgeServer};
pub use command::Command;
#[doc(hidden)]
pub use pipe::PipeHandle;
pub use spawn::{spawn_with_elevated_privileges, spawn_with_normal_privileges};
pub use token::PrivilegeLevel;
pub use token::Token;

fn win32_error_with_context(context: &str, err: IoError) -> IoError {
    IoError::new(err.kind(), format!("{}: {}", context, err))
}

fn os_str_to_null_terminated_vec(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(std::iter::once(0)).collect()
}
