use deelevate::{BridgePtyClient, Command, PipeHandle, Token};
use std::convert::TryInto;
use std::ffi::OsString;
use std::path::PathBuf;
use structopt::*;
use winapi::um::wincon::{SetConsoleCP, SetConsoleCursorPosition, SetConsoleOutputCP, COORD};
use winapi::um::winnls::CP_UTF8;

/// A helper program for `eledo` and `normdo` that is used to
/// bridge pty and pipes between the different privilege levels.
/// This utility is not intended to be run by humans.
#[derive(StructOpt)]
#[structopt(
    version = env!("VERGEN_SEMVER_LIGHTWEIGHT")
)]
struct Opt {
    #[structopt(long, parse(from_os_str))]
    stdin: Option<PathBuf>,
    #[structopt(long, parse(from_os_str))]
    stdout: Option<PathBuf>,
    #[structopt(long, parse(from_os_str))]
    stderr: Option<PathBuf>,
    #[structopt(long, parse(from_os_str))]
    conin: Option<PathBuf>,
    #[structopt(long, parse(from_os_str))]
    conout: Option<PathBuf>,

    #[structopt(long)]
    width: Option<usize>,
    #[structopt(long)]
    height: Option<usize>,

    #[structopt(long)]
    cursor_x: Option<usize>,
    #[structopt(long)]
    cursor_y: Option<usize>,

    #[structopt(parse(from_os_str))]
    args: Vec<OsString>,
}

fn main() -> std::io::Result<()> {
    let mut opt = Opt::from_args();

    unsafe {
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
    }

    let token = Token::with_current_process()?;

    if let Some(conin) = opt.conin {
        let pty_client = BridgePtyClient::with_params(
            &conin,
            &opt.conout.unwrap(),
            opt.width.unwrap(),
            opt.height.unwrap(),
        )?;

        let mut args: Vec<OsString> = vec![std::env::current_exe()?.into()];

        if let Some(stdin) = opt.stdin {
            args.push("--stdin".into());
            args.push(stdin.into());
        }
        if let Some(stdout) = opt.stdout {
            args.push("--stdout".into());
            args.push(stdout.into());
        }
        if let Some(stderr) = opt.stderr {
            args.push("--stderr".into());
            args.push(stderr.into());
        }
        if let Some(cursor_x) = opt.cursor_x {
            args.push("--cursor-x".into());
            args.push(cursor_x.to_string().into());
        }
        if let Some(cursor_y) = opt.cursor_x {
            args.push("--cursor-y".into());
            args.push(cursor_y.to_string().into());
        }

        args.push("--".into());
        args.append(&mut opt.args);

        let mut cmd = Command::with_environment_for_token(&token)?;
        cmd.set_argv(args);

        let exit_code = pty_client.run(cmd)?;
        std::process::exit(exit_code as _);
    } else {
        let mut cmd = Command::with_environment_for_token(&token)?;
        cmd.set_argv(opt.args);

        if let Some(stdin) = opt.stdin {
            cmd.set_stdin(PipeHandle::open_pipe(stdin)?)?;
        }
        if let Some(stdout) = opt.stdout {
            cmd.set_stdout(PipeHandle::open_pipe(stdout)?)?;
        }
        if let Some(stderr) = opt.stderr {
            cmd.set_stderr(PipeHandle::open_pipe(stderr)?)?;
        }

        if let Some(cursor_x) = opt.cursor_x {
            let conout = PipeHandle::open_pipe("CONOUT$")?;
            unsafe {
                SetConsoleCursorPosition(
                    conout.as_handle(),
                    COORD {
                        X: cursor_x.try_into().unwrap(),
                        Y: opt.cursor_y.unwrap().try_into().unwrap(),
                    },
                );
            }
        }

        let proc = cmd.spawn()?;
        let _ = proc.wait_for(None)?;
        let exit_code = proc.exit_code()?;
        std::process::exit(exit_code as _);
    }
}
