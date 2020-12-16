use crate::bridge::BridgeServer;
use crate::command::*;
use crate::{PrivilegeLevel, Token};
use std::convert::TryInto;
use std::io::Result as IoResult;

/// Spawn a copy of the current process using the provided token.
/// The existing streams are passed through to the child.
/// On success, does not return to the caller; it will terminate
/// the current process and assign the exit status from the child.
fn spawn_with_current_io_streams(token: &Token) -> IoResult<()> {
    let mut cmd = Command::with_environment_for_token(token)?;
    cmd.set_command_from_current_process()?;
    let proc = cmd.spawn_as_user(token)?;

    proc.wait_for(None)?;

    let exit_code = proc.exit_code()?;
    std::process::exit(exit_code.try_into().unwrap());
}

/// If the token is PrivilegeLevel::NotPrivileged then this function
/// will return `Ok` and the intent is that the host program continue
/// with its normal operation.
///
/// Otherwise, assuming no errors were detected, this function will
/// not return to the caller.  Instead a reduced privilege token
/// will be created and used to spawn a copy of the host program,
/// passing through the arguments from the current process.
/// *This* process will remain running to bridge pipes for the stdio
/// streams to the new process and to wait for the child process
/// and then terminate *this* process and exit with the exit code
/// from the child.
pub fn spawn_with_normal_privileges() -> IoResult<()> {
    let token = Token::with_current_process()?;
    let level = token.privilege_level()?;

    match level {
        PrivilegeLevel::NotPrivileged => Ok(()),
        PrivilegeLevel::Elevated => {
            let target_token = Token::with_shell_process()?;
            let mut server = BridgeServer::new();
            let mut argv = std::env::args_os().collect();
            let mut bridge_cmd = server.start_for_command(&mut argv, &target_token)?;
            let proc = bridge_cmd.spawn_with_token(&target_token)?;
            std::process::exit(server.serve(proc)? as _);
        }
        PrivilegeLevel::HighIntegrityAdmin => {
            let medium_token = token.as_medium_integrity_safer_token()?;
            spawn_with_current_io_streams(&medium_token)
        }
    }
}

/// If the token is NOT PrivilegeLevel::NotPrivileged then this function
/// will return `Ok` and the intent is that the host program continue
/// with its normal operation.
///
/// Otherwise, assuming no errors were detected, this function will
/// not return to the caller.  Instead an elevated privilege token
/// will be created and used to spawn a copy of the host program,
/// passing through the arguments from the current process.
/// *This* process will remain running to bridge pipes for the stdio
/// streams to the new process and to wait for the child process
/// and then terminate *this* process and exit with the exit code
/// from the child.
pub fn spawn_with_elevated_privileges() -> IoResult<()> {
    let token = Token::with_current_process()?;
    let level = token.privilege_level()?;

    let target_token = match level {
        PrivilegeLevel::NotPrivileged => token.as_medium_integrity_safer_token()?,
        PrivilegeLevel::HighIntegrityAdmin | PrivilegeLevel::Elevated => return Ok(()),
    };

    let mut server = BridgeServer::new();
    let mut argv = std::env::args_os().collect();
    let mut bridge_cmd = server.start_for_command(&mut argv, &target_token)?;
    let proc = bridge_cmd.spawn_with_token(&target_token)?;
    std::process::exit(server.serve(proc)? as _);
}

/// This function is for use by C/C++ code that wants to test whether the
/// current session is elevated.  The return value is 0 for a non-privileged
/// process and non-zero for a privileged process.
/// If an error occurs while obtaining this information, the program will
/// terminate.
#[no_mangle]
pub extern "C" fn deelevate_is_privileged_process() -> i32 {
    match Token::with_current_process().and_then(|token| token.privilege_level()) {
        Ok(PrivilegeLevel::Elevated) | Ok(PrivilegeLevel::HighIntegrityAdmin) => 1,
        Ok(PrivilegeLevel::NotPrivileged) => 0,
        Err(e) => {
            eprintln!(
                "An error occurred while determining the privilege level: {}",
                e
            );
            std::process::exit(1);
        }
    }
}

/// This function is for use by C/C++ code that wants to ensure that execution
/// will only continue if the current token has a Normal privilege level.
/// This function will attempt to re-execute the program in the appropriate
/// context.
/// This function will only return if the current context has normal privs.
#[no_mangle]
pub extern "C" fn deelevate_requires_normal_privileges() {
    if let Err(e) = spawn_with_normal_privileges() {
        eprintln!(
            "This program requires running with Normal privileges and \
                  encountered an issue while attempting to run in that context: {}",
            e
        );
        std::process::exit(1);
    }
}

/// This function is for use by C/C++ code that wants to ensure that execution
/// will only continue if the current token has an Elevated privilege level.
/// This function will attempt to re-execute the program in the appropriate
/// context.
/// This function will only return if the current context has Elevated or
/// High Integrity Admin privs.
#[no_mangle]
pub extern "C" fn deelevate_requires_elevated_privileges() {
    if let Err(e) = spawn_with_elevated_privileges() {
        eprintln!(
            "This program requires running with Elevated privileges and \
                  encountered an issue while attempting to run in that context: {}",
            e
        );
        std::process::exit(1);
    }
}
