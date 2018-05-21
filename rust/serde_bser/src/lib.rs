extern crate byteorder;
extern crate bytes;
#[macro_use]
extern crate error_chain;
#[cfg(test)]
#[macro_use]
extern crate maplit;
#[macro_use]
extern crate serde;
extern crate serde_bytes;
#[macro_use]
extern crate serde_derive;

pub mod de;
mod errors;
mod header;
pub mod ser;

pub use de::from_reader;
pub use de::from_slice;
