use std::fmt;

use error_chain::error_chain;
use serde::{de, ser};

use crate::header::header_byte_desc;

error_chain! {
    errors {
        DeInvalidStartByte(kind: String, byte: u8) {
            description("while deserializing BSER: invalid start byte")
            display("while deserializing BSER: invalid start byte for {}: {}",
                    kind, header_byte_desc(*byte))
        }
        DeCustom(msg: String) {
            description("error while deserializing BSER")
            display("error while deserializing BSER: {}", msg)
        }
        DeRecursionLimitExceeded(kind: String) {
            description("while deserializing BSER: recursion limit exceeded")
            display("while deserializing BSER: recursion limit exceeded with {}", kind)
        }
        SerCustom(msg: String) {
            description("error while serializing BSER")
            display("error while serializing BSER: {}", msg)
        }
        SerNeedSize(kind: &'static str) {
            description("while serializing BSER: need size")
            display("while serializing BSER: need size of {}", kind)
        }
        SerU64TooBig(v: u64) {
            description("while serializing BSER: integer too big")
            display("while serializing BSER: integer too big: {}", v)
        }
    }

    foreign_links {
        Io(::std::io::Error);
        Utf8(::std::str::Utf8Error);
    }
}

impl de::Error for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        ErrorKind::DeCustom(format!("{}", msg)).into()
    }
}

impl ser::Error for Error {
    fn custom<T: fmt::Display>(msg: T) -> Self {
        ErrorKind::SerCustom(format!("{}", msg)).into()
    }
}
