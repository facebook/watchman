use crate::command::Command;
use crate::pipe::*;
use crate::process::Process;
use crate::psuedocon::PsuedoCon;
use crate::win32_error_with_context;
use crate::Token;
use std::ffi::{OsStr, OsString};
use std::io::{Error as IoError, Read, Result as IoResult, Write};
use std::os::windows::prelude::*;
use std::path::{Path, PathBuf};
use winapi::shared::minwindef::DWORD;
use winapi::um::consoleapi::{GetConsoleMode, SetConsoleMode};
use winapi::um::consoleapi::{ReadConsoleW, WriteConsoleW};
use winapi::um::fileapi::GetFileType;
use winapi::um::winbase::FILE_TYPE_CHAR;
use winapi::um::wincon::{
    GetConsoleScreenBufferInfo, CONSOLE_SCREEN_BUFFER_INFO, DISABLE_NEWLINE_AUTO_RETURN,
    ENABLE_PROCESSED_OUTPUT, ENABLE_VIRTUAL_TERMINAL_INPUT, ENABLE_VIRTUAL_TERMINAL_PROCESSING,
    ENABLE_WRAP_AT_EOL_OUTPUT,
};
use winapi::um::wincontypes::COORD;

pub struct BridgePtyClient {
    con: PsuedoCon,
}

impl BridgePtyClient {
    pub fn with_params(conin: &Path, conout: &Path, width: usize, height: usize) -> IoResult<Self> {
        let client_to_server = PipeHandle::open_pipe(conout)?;
        let server_to_client = PipeHandle::open_pipe(conin)?;

        let con = PsuedoCon::new(
            COORD {
                X: width as i16,
                Y: height as i16,
            },
            server_to_client,
            client_to_server,
        )?;

        Ok(Self { con })
    }

    pub fn run(&self, mut command: Command) -> IoResult<DWORD> {
        let proc = command.spawn_with_pty(&self.con)?;
        proc.wait_for(None)?;
        // Well, this is a bit awkward.
        // If we kill the pty immediately when the child process exits,
        // it will fracture the associated pipes and any buffered
        // output will be lost.
        // There doesn't seem to be a reasonable way to wait for that
        // flush to occur from here, so we just use a short sleep.
        // This is gross and I wonder if this is even long enough
        // for every case?
        std::thread::sleep(std::time::Duration::from_millis(300));
        proc.exit_code()
    }
}

#[allow(unused)]
fn join_with_timeout(join_handle: std::thread::JoinHandle<()>, timeout: std::time::Duration) {
    use std::sync::mpsc::channel;
    let (tx, rx) = channel();
    std::thread::spawn(move || {
        let _ = join_handle.join();
        let _ = tx.send(());
    });
    let _ = rx.recv_timeout(timeout);
}

/// The bridge server is the originator of the spawned command.
/// It owns the server end of the connection and awaits the
/// bridge client connection.
pub struct BridgeServer {
    stdin_is_pty: bool,
    stdout_is_pty: bool,
    stderr_is_pty: bool,

    stdin: Option<PipeHandle>,
    stdout: Option<PipeHandle>,
    stderr: Option<PipeHandle>,

    conin: Option<PipeHandle>,
    conin_pipe: Option<PipeHandle>,
    conout: Option<PipeHandle>,
    conout_pipe: Option<PipeHandle>,

    input_mode: Option<DWORD>,
    output_mode: Option<DWORD>,
}

impl Drop for BridgeServer {
    fn drop(&mut self) {
        if let Some(mode) = self.output_mode {
            if let Ok(mut conout) = PipeHandle::open_pipe("CONOUT$") {
                // Emit a soft reset
                let _ = write!(&mut conout, "\x1b[!p");
                // Restore mode
                let _ = set_console_mode(&conout, mode);
            }
        }
        if let Some(mode) = self.input_mode {
            if let Ok(conin) = PipeHandle::open_pipe("CONIN$") {
                let _ = set_console_mode(&conin, mode);
            }
        }
    }
}

fn get_console_mode(pipe: &PipeHandle) -> IoResult<DWORD> {
    let mut mode = 0;
    let res = unsafe { GetConsoleMode(pipe.as_handle(), &mut mode) };
    if res == 0 {
        Err(win32_error_with_context(
            "GetConsoleMode",
            IoError::last_os_error(),
        ))
    } else {
        Ok(mode)
    }
}

fn set_console_mode(pipe: &PipeHandle, mode: DWORD) -> IoResult<()> {
    let res = unsafe { SetConsoleMode(pipe.as_handle(), mode) };
    if res == 0 {
        Err(win32_error_with_context(
            "SetConsoleMode",
            IoError::last_os_error(),
        ))
    } else {
        Ok(())
    }
}

// Due to https://github.com/microsoft/terminal/issues/4551
// we cannot simply write bytes to the console, we have to
// use WriteConsoleW to send the unicode data through, otherwise
// we end up with problems with mismatching codepages.
fn write_console(out: &mut PipeHandle, s: &str) -> IoResult<()> {
    let c: Vec<u16> = OsStr::new(s).encode_wide().collect();
    let mut wrote = 0;
    let res = unsafe {
        WriteConsoleW(
            out.as_handle(),
            c.as_ptr() as *const _,
            c.len() as _,
            &mut wrote,
            std::ptr::null_mut(),
        )
    };
    if res == 0 {
        Err(IoError::last_os_error())
    } else {
        Ok(())
    }
}

fn is_pty_stream<F: AsRawHandle>(f: &F) -> bool {
    let handle = f.as_raw_handle();
    unsafe { GetFileType(handle as _) == FILE_TYPE_CHAR }
}

impl BridgeServer {
    pub fn new() -> Self {
        let stdin_is_pty = is_pty_stream(&std::io::stdin());
        let stdout_is_pty = is_pty_stream(&std::io::stdout());
        let stderr_is_pty = is_pty_stream(&std::io::stderr());

        Self {
            stdin_is_pty,
            stdout_is_pty,
            stderr_is_pty,
            conin: None,
            conout: None,
            conin_pipe: None,
            conout_pipe: None,
            input_mode: None,
            output_mode: None,
            stderr: None,
            stdout: None,
            stdin: None,
        }
    }

    pub fn start_for_command(
        &mut self,
        argv: &mut Vec<OsString>,
        target_token: &Token,
    ) -> IoResult<Command> {
        let bridge_path = locate_pty_bridge()?;
        let mut bridge_args = self.start(target_token)?;

        bridge_args.insert(0, bridge_path.into_os_string());
        bridge_args.push("--".into());
        bridge_args.append(argv);

        let mut bridge_cmd = Command::with_environment_for_token(&target_token)?;
        bridge_cmd.set_argv(bridge_args);
        bridge_cmd.hide_window();

        Ok(bridge_cmd)
    }

    /// Creates the server pipe and returns the name of the pipe
    /// so that it can be passed to the client process
    pub fn start(&mut self, token: &Token) -> IoResult<Vec<OsString>> {
        let mut args = vec![];

        if !self.stdin_is_pty {
            let pipe = NamedPipeServer::for_token(token)?;
            self.stdin.replace(pipe.pipe);
            args.push("--stdin".into());
            args.push(pipe.path.into());
        }

        if !self.stdout_is_pty {
            let pipe = NamedPipeServer::for_token(token)?;
            self.stdout.replace(pipe.pipe);
            args.push("--stdout".into());
            args.push(pipe.path.into());
        }

        if !self.stderr_is_pty {
            let pipe = NamedPipeServer::for_token(token)?;
            self.stderr.replace(pipe.pipe);
            args.push("--stderr".into());
            args.push(pipe.path.into());
        }

        if let Ok(conin) = PipeHandle::open_pipe("CONIN$") {
            self.input_mode.replace(get_console_mode(&conin)?);
            let pipe = NamedPipeServer::for_token(token)?;
            self.conin_pipe.replace(pipe.pipe);

            args.push("--conin".into());
            args.push(pipe.path.into());

            set_console_mode(
                &conin,
                // ENABLE_PROCESSED_OUTPUT |  FIXME: CTRl-C handling?
                ENABLE_VIRTUAL_TERMINAL_INPUT,
            )?;
            self.conin.replace(conin);
        }

        if let Ok(conout) = PipeHandle::open_pipe("CONOUT$") {
            self.output_mode.replace(get_console_mode(&conout)?);
            let pipe = NamedPipeServer::for_token(token)?;
            self.conout_pipe.replace(pipe.pipe);

            args.push("--conout".into());
            args.push(pipe.path.into());

            let mut console_info: CONSOLE_SCREEN_BUFFER_INFO = unsafe { std::mem::zeroed() };
            let res = unsafe { GetConsoleScreenBufferInfo(conout.as_handle(), &mut console_info) };

            if res == 0 {
                return Err(win32_error_with_context(
                    "GetConsoleScreenBufferInfo",
                    IoError::last_os_error(),
                ));
            }

            // The console info describes the buffer dimensions.
            // We need to do a little bit of math to obtain the viewport dimensions!
            let width = console_info
                .srWindow
                .Right
                .saturating_sub(console_info.srWindow.Left) as usize
                + 1;

            args.push("--width".into());
            args.push(width.to_string().into());

            let height = console_info
                .srWindow
                .Bottom
                .saturating_sub(console_info.srWindow.Top) as usize
                + 1;

            args.push("--height".into());
            args.push(height.to_string().into());

            let cursor_x = console_info.dwCursorPosition.X as usize;
            let cursor_y = console_info
                .dwCursorPosition
                .Y
                .saturating_sub(console_info.srWindow.Top) as usize;

            args.push("--cursor-x".into());
            args.push(cursor_x.to_string().into());

            args.push("--cursor-y".into());
            args.push(cursor_y.to_string().into());

            set_console_mode(
                &conout,
                ENABLE_PROCESSED_OUTPUT
                    | ENABLE_WRAP_AT_EOL_OUTPUT
                    | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                    | DISABLE_NEWLINE_AUTO_RETURN,
            )?;

            self.conout.replace(conout);
        }

        Ok(args)
    }

    pub fn serve(mut self, proc: Process) -> IoResult<DWORD> {
        if let Some(conin) = self.conin.take() {
            let mut conin_dest = self.conin_pipe.take().unwrap();
            conin_dest.wait_for_pipe_client()?;
            std::thread::spawn(move || -> IoResult<()> {
                let mut buf = [0u16; 8192];
                let mut num_read = 0;
                loop {
                    let res = unsafe {
                        ReadConsoleW(
                            conin.as_handle(),
                            buf.as_mut_ptr() as *mut _,
                            buf.len() as _,
                            &mut num_read,
                            std::ptr::null_mut(),
                        )
                    };

                    if res == 0 {
                        return Err(IoError::last_os_error());
                    }

                    let s = OsString::from_wide(&buf[0..num_read as usize]);
                    let utf8 = s.to_string_lossy();

                    conin_dest.write_all(utf8.as_bytes())?;
                }
            });
        }

        // Start up the console output processing thread.
        // This is ostensibly just a matter of taking the output
        // from the pty created by the bridge executable and piping it
        // into our own CONOUT$ stream, but it is made a little bit
        // more complicated because the Windows console APIs emit
        // some slightly hostile initialization sequences when creating
        // a fresh PTY and launching a process inside it: it will emit
        // sequences that move the cursor, clear the screen and change
        // the window title sequence.
        // For our embedding use case those are distinctly unwanted.
        // In order to deal with this, we need to parse the terminal
        // output so that we can filter them out.
        // The approach is simple: until we spot that initial title
        // change, we'll filter out CSI and OSC sequences.
        // Just in case the behavior changes in the future, we'll
        // also disable suppression if we see any other kind of
        // output from the pty stream.
        let conout_thread = self.conout.take().map(|mut conout| {
            let mut conout_src = self.conout_pipe.take().unwrap();
            let _ = conout_src.wait_for_pipe_client();
            std::thread::spawn(move || -> IoResult<()> {
                use termwiz::escape::osc::OperatingSystemCommand;
                use termwiz::escape::parser::Parser;
                use termwiz::escape::Action;

                let mut parser = Parser::new();

                let mut buf = [0u8; 4096];
                let mut suppress_control = true;

                loop {
                    let len = conout_src.read(&mut buf)?;
                    if len == 0 {
                        return Ok(());
                    }

                    let mut error = None;
                    let mut callback = |action: Action| -> IoResult<()> {
                        match action {
                            Action::OperatingSystemCommand(osc) => {
                                match *osc {
                                    OperatingSystemCommand::SetIconNameAndWindowTitle(_) => {
                                        if suppress_control {
                                            // We're now sync'd up with the new pty instance.
                                            // We ignore this first title change request because
                                            // it is going to be the uninteresting bridge exe
                                            suppress_control = false;
                                            Ok(())
                                        } else {
                                            write_console(&mut conout, &format!("{}", osc))
                                        }
                                    }
                                    _ => write_console(&mut conout, &format!("{}", osc)),
                                }
                            }
                            Action::CSI(c) => {
                                if !suppress_control {
                                    write_console(&mut conout, &format!("{}", c))
                                } else {
                                    Ok(())
                                }
                            }
                            _ => {
                                suppress_control = false;
                                write_console(&mut conout, &format!("{}", action))
                            }
                        }
                    };

                    parser.parse(&buf[0..len], |action| {
                        if let Err(e) = callback(action) {
                            error.replace(e);
                        }
                    });

                    if let Some(e) = error.take() {
                        return Err(e);
                    }
                }
            })
        });

        if let Some(mut stdin_dest) = self.stdin.take() {
            stdin_dest.wait_for_pipe_client()?;
            std::thread::spawn(move || {
                let mut stdin = std::io::stdin();
                let _ = std::io::copy(&mut stdin, &mut stdin_dest);
            });
        }

        let stdout_thread = self.stdout.take().map(|mut stdout_src| {
            let _ = stdout_src.wait_for_pipe_client();
            std::thread::spawn(move || {
                let mut stdout = std::io::stdout();
                let _ = std::io::copy(&mut stdout_src, &mut stdout);
            })
        });
        let stderr_thread = self.stderr.take().map(|mut stderr_src| {
            let _ = stderr_src.wait_for_pipe_client();
            std::thread::spawn(move || {
                let mut stderr = std::io::stderr();
                let _ = std::io::copy(&mut stderr_src, &mut stderr);
            })
        });

        let _ = proc.wait_for(None)?;

        stdout_thread.map(|t| t.join());
        stderr_thread.map(|t| t.join());
        conout_thread.map(|t| t.join());

        let exit_code = proc.exit_code()?;
        Ok(exit_code)
    }
}

fn locate_pty_bridge() -> IoResult<PathBuf> {
    let bridge_name = "eledo-pty-bridge.exe";
    let bridge_path = std::env::current_exe()?
        .parent()
        .ok_or_else(|| {
            std::io::Error::new(
                std::io::ErrorKind::Other,
                "current exe has no containing dir while locating pty bridge!?",
            )
        })?
        .join(bridge_name);
    if bridge_path.exists() {
        Ok(bridge_path)
    } else {
        pathsearch::find_executable_in_path(bridge_name).ok_or_else(|| {
            std::io::Error::new(
                std::io::ErrorKind::Other,
                format!(
                    "{} not found alongside executable or in the path",
                    bridge_name
                ),
            )
        })
    }
}
