#![cfg(windows)]
use crate::Error;
use std::io::prelude::*;
use std::io::Error as IoError;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::io::FromRawHandle;
use std::path::PathBuf;
use std::pin::Pin;
use std::task::{Context, Poll};
use tokio::prelude::*;
use tokio_net::util::PollEvented;
use winapi::um::fileapi::*;
use winapi::um::winbase::*;
use winapi::um::winnt::*;

/// Spiritually similar in intent to the tokio-named-pipes crate
/// equivalent, but this implementation works with pipe clients
/// rather than servers, and works with the new async/await
/// futures impl
pub struct NamedPipe {
    io: PollEvented<mio_named_pipes::NamedPipe>,
}

impl NamedPipe {
    pub async fn connect(path: PathBuf) -> Result<Self, Error> {
        let win_path = path
            .as_os_str()
            .encode_wide()
            .chain(Some(0))
            .collect::<Vec<_>>();

        let handle = unsafe {
            CreateFileW(
                win_path.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null_mut(),
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                std::ptr::null_mut(),
            )
        };
        if handle.is_null() {
            let err = IoError::last_os_error();
            return Err(Error::Connect {
                endpoint: path,
                source: Box::new(err),
            });
        }

        let pipe = unsafe { mio_named_pipes::NamedPipe::from_raw_handle(handle) };
        let io = PollEvented::new(pipe);
        Ok(Self { io })
    }
}

impl AsyncRead for NamedPipe {
    fn poll_read(
        self: Pin<&mut Self>,
        ctx: &mut Context,
        buf: &mut [u8],
    ) -> Poll<Result<usize, IoError>> {
        match self.io.poll_read_ready(ctx, mio::Ready::readable()) {
            Poll::Ready(res) => res?,
            Poll::Pending => return Poll::Pending,
        };

        match self.io.get_ref().read(buf) {
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                self.io.clear_read_ready(ctx, mio::Ready::readable())?;
                Poll::Pending
            }

            x => Poll::Ready(x),
        }
    }
}

impl AsyncWrite for NamedPipe {
    fn poll_write(
        self: Pin<&mut Self>,
        ctx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, IoError>> {
        match self.io.poll_write_ready(ctx) {
            Poll::Ready(res) => res?,
            Poll::Pending => return Poll::Pending,
        };

        match self.io.get_ref().write(buf) {
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                self.io.clear_write_ready(ctx)?;
                Poll::Pending
            }

            x => Poll::Ready(x),
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _ctx: &mut Context) -> Poll<Result<(), IoError>> {
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, _ctx: &mut Context) -> Poll<Result<(), IoError>> {
        Poll::Ready(Ok(()))
    }
}

impl crate::ReadWriteStream for NamedPipe {}
