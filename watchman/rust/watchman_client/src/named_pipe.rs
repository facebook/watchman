#![cfg(windows)]
/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use crate::Error;
use std::io::Error as IoError;
use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;
use std::pin::Pin;
use std::task::{Context, Poll};
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio::net::windows::named_pipe::NamedPipeClient;
use winapi::um::fileapi::{CreateFileW, OPEN_EXISTING};
use winapi::um::winbase::FILE_FLAG_OVERLAPPED;
use winapi::um::winnt::{GENERIC_READ, GENERIC_WRITE};

/// Wrapper around a tokio [`NamedPipeClient`]
pub struct NamedPipe {
    io: NamedPipeClient,
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

        let io = unsafe {
            NamedPipeClient::from_raw_handle(handle).map_err(|err| Error::Connect {
                endpoint: path,
                source: Box::new(err),
            })?
        };
        Ok(Self { io })
    }
}

impl AsyncRead for NamedPipe {
    fn poll_read(
        self: Pin<&mut Self>,
        ctx: &mut Context,
        buf: &mut ReadBuf,
    ) -> Poll<Result<(), IoError>> {
        AsyncRead::poll_read(Pin::new(&mut self.get_mut().io), ctx, buf)
    }
}

impl AsyncWrite for NamedPipe {
    fn poll_write(
        self: Pin<&mut Self>,
        ctx: &mut Context,
        buf: &[u8],
    ) -> Poll<Result<usize, IoError>> {
        AsyncWrite::poll_write(Pin::new(&mut self.get_mut().io), ctx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, ctx: &mut Context) -> Poll<Result<(), IoError>> {
        AsyncWrite::poll_flush(Pin::new(&mut self.get_mut().io), ctx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, ctx: &mut Context) -> Poll<Result<(), IoError>> {
        AsyncWrite::poll_shutdown(Pin::new(&mut self.get_mut().io), ctx)
    }
}

impl crate::ReadWriteStream for NamedPipe {}
