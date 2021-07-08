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
