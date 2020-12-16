use deelevate::{BridgeServer, Command, PrivilegeLevel, Token};
use pathsearch::find_executable_in_path;
use std::ffi::OsString;
use structopt::*;

/// NormDo - "Do" a command with Normal privileges
///
/// NormDo will check to see if the current context has admin privileges.
/// If it doesn't then it will execute the requested program directly,
/// returning the exit status from that program.
///
/// Otherwise, NormDo will arrange to run the program with a Normal
/// user token with Medium integrity level, dropping/denying the local
/// administrator group from the token.  The program will be run in a
/// PTY that is bridged to the current terminal session.
///
/// Example:
///    `normdo whoami /groups`
#[derive(StructOpt)]
#[structopt(
    about = "NormDo - \"Do\" a command with Normal privileges",
    author = "Wez Furlong",
    setting(clap::AppSettings::TrailingVarArg),
    setting(clap::AppSettings::ArgRequiredElseHelp),
    version = env!("VERGEN_SEMVER_LIGHTWEIGHT")
)]
#[derive(Debug)]
struct Opt {
    #[structopt(value_name("PROGRAM"), parse(from_os_str))]
    args: Vec<OsString>,
}

fn main() -> std::io::Result<()> {
    let mut opt = Opt::from_args();

    let token = Token::with_current_process()?;
    let level = token.privilege_level()?;

    opt.args[0] = match find_executable_in_path(&opt.args[0]) {
        Some(path) => path.into(),
        None => {
            eprintln!("Unable to find {:?} in path", opt.args[0]);
            std::process::exit(1);
        }
    };

    let target_token = match level {
        PrivilegeLevel::NotPrivileged => token,
        PrivilegeLevel::HighIntegrityAdmin => token.as_medium_integrity_safer_token()?,
        PrivilegeLevel::Elevated => Token::with_shell_process()?,
    };

    let mut command = Command::with_environment_for_token(&target_token)?;

    let exit_code = match level {
        PrivilegeLevel::NotPrivileged => {
            // We're already normal, so just run it directly
            command.set_argv(opt.args);
            let proc = command.spawn()?;
            let _ = proc.wait_for(None);
            proc.exit_code()?
        }
        PrivilegeLevel::HighIntegrityAdmin | PrivilegeLevel::Elevated => {
            let mut server = BridgeServer::new();

            let mut bridge_cmd = server.start_for_command(&mut opt.args, &target_token)?;

            let proc = match level {
                PrivilegeLevel::Elevated => bridge_cmd.spawn_with_token(&target_token)?,
                PrivilegeLevel::NotPrivileged | PrivilegeLevel::HighIntegrityAdmin => {
                    bridge_cmd.spawn_as_user(&target_token)?
                }
            };

            server.serve(proc)?
        }
    };

    std::process::exit(exit_code as _);
}
