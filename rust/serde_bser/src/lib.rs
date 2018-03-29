extern crate byteorder;
extern crate bytes;
#[macro_use]
extern crate error_chain;
#[macro_use]
extern crate serde;
extern crate serde_bytes;
#[macro_use]
extern crate serde_derive;

pub mod de;
pub mod ser;
mod errors;
mod header;

pub use de::from_slice;
