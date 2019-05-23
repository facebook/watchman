extern crate byteorder;
extern crate bytes;
#[macro_use]
extern crate error_chain;
#[cfg(test)]
#[macro_use]
extern crate maplit;
#[macro_use]
extern crate serde;

#[macro_use]
extern crate serde_derive;

pub mod de;
mod errors;
mod header;
pub mod ser;

pub use crate::de::from_reader;
pub use crate::de::from_slice;
