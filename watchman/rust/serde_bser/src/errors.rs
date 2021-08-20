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

use std::fmt;

use serde::{de, ser};
use thiserror::Error;

use crate::header::header_byte_desc;

pub type Result<T> = ::std::result::Result<T, Error>;

#[derive(Error, Debug)]
pub enum Error {
    #[error("while deserializing BSER: invalid start byte for {}: {}", .kind, header_byte_desc(*.byte))]
    DeInvalidStartByte { kind: String, byte: u8 },

    #[error("error while deserializing BSER: {}", msg)]
    DeCustom { msg: String },

    #[error("while deserializing BSER: recursion limit exceeded with {}", .kind)]
    DeRecursionLimitExceeded { kind: String },

    #[error("Expected {} bytes read, but only read {} bytes", .expected, .read)]
    DeEof { expected: usize, read: usize },

    #[error("Invalid magic header: {:?}", .magic)]
    DeInvalidMagic { magic: Vec<u8> },

    #[error("reader error while deserializing")]
    DeReaderError {
        #[source]
        source: anyhow::Error,
    },

    #[error("error while serializing BSER: {}", .msg)]
    SerCustom { msg: String },

    #[error("while serializing BSER: need size of {}", .kind)]
    SerNeedSize { kind: &'static str },

    #[error("while serializing BSER: integer too big: {}", .v)]
    SerU64TooBig { v: u64 },

    #[error("IO Error")]
    Io(#[from] ::std::io::Error),
}

impl Error {
    pub fn de_reader_error(source: anyhow::Error) -> Self {
        Self::DeReaderError { source }
    }
}

impl de::Error for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        Error::DeCustom {
            msg: format!("{}", msg),
        }
    }
}

impl ser::Error for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        Error::SerCustom {
            msg: format!("{}", msg),
        }
    }
}
