#![deny(warnings, rust_2018_idioms)]

pub mod bytestring;
pub mod de;
mod errors;
mod header;
pub mod ser;
pub mod value;

pub use crate::de::from_reader;
pub use crate::de::from_slice;
