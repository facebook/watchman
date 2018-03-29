use std::fmt;
use std::result;
use std::str;

use byteorder::{ByteOrder, NativeEndian};
use errors::*;

pub trait DeRead<'de>: fmt::Debug {
    fn next(&mut self) -> Result<u8>;
    fn peek(&mut self) -> Result<u8>;
    /// Verify that at least `len` bytes are available, if possible.
    fn verify_remaining(&self, len: usize) -> Result<()>;
    /// Verify that exactly `len` bytes have been read.
    fn verify_read_count(&self, len: usize) -> Result<()>;
    /// How many bytes have been read so far.
    fn read_count(&self) -> usize;

    fn discard(&mut self);

    fn next_bytes<'s>(
        &'s mut self,
        len: usize,
        scratch: &'s mut Vec<u8>,
    ) -> Result<Reference<'de, 's, [u8]>>;

    fn next_u32(&mut self, scratch: &mut Vec<u8>) -> Result<u32> {
        let bytes = self.next_bytes(4, scratch)
            .chain_err(|| "error while parsing u32")?
            .get_ref();
        Ok(NativeEndian::read_u32(bytes))
    }
}

#[derive(Debug)]
pub struct SliceRead<'a> {
    slice: &'a [u8],
    index: usize,
}

impl<'a> SliceRead<'a> {
    pub fn new(slice: &'a [u8]) -> Self {
        SliceRead {
            slice: slice,
            index: 0,
        }
    }
}

impl<'a> DeRead<'a> for SliceRead<'a> {
    fn next(&mut self) -> Result<u8> {
        if self.index >= self.slice.len() {
            bail!("eof while reading next byte");
        }
        let ch = self.slice[self.index];
        self.index += 1;
        Ok(ch)
    }

    fn peek(&mut self) -> Result<u8> {
        if self.index >= self.slice.len() {
            bail!("eof while peeking next byte");
        }
        Ok(self.slice[self.index])
    }

    fn verify_remaining(&self, len: usize) -> Result<()> {
        let actual_len = self.slice.len() - self.index;
        if actual_len < len {
            bail!(
                "Expected at least {} bytes, but only {} bytes remain in slice",
                len,
                actual_len
            );
        }
        Ok(())
    }

    fn verify_read_count(&self, len: usize) -> Result<()> {
        if self.index != len {
            bail!(
                "Expected {} bytes read, but only read {} bytes",
                len,
                self.index
            );
        }
        Ok(())
    }

    fn read_count(&self) -> usize {
        self.index
    }

    #[inline]
    fn discard(&mut self) {
        self.index += 1;
    }

    fn next_bytes<'s>(
        &'s mut self,
        len: usize,
        _scratch: &'s mut Vec<u8>,
    ) -> Result<Reference<'a, 's, [u8]>> {
        // BSER has no escaping or anything similar, so just go ahead and return
        // a reference to the bytes.
        if self.index + len > self.slice.len() {
            bail!("eof while parsing bytes/string");
        }
        let borrowed = &self.slice[self.index..(self.index + len)];
        self.index += len;
        Ok(Reference::Borrowed(borrowed))
    }
}

#[derive(Debug)]
pub enum Reference<'b, 'c, T>
where
    T: ?Sized + 'b + 'c,
{
    Borrowed(&'b T),
    Copied(&'c T),
}

impl<'b, 'c, T> Reference<'b, 'c, T>
where
    T: ?Sized + 'b + 'c,
{
    pub fn map_result<F, U, E>(self, f: F) -> Result<Reference<'b, 'c, U>>
    where
        F: FnOnce(&T) -> result::Result<&U, E>,
        Error: From<E>,
        U: ?Sized + 'b + 'c,
    {
        match self {
            Reference::Borrowed(borrowed) => Ok(Reference::Borrowed(f(borrowed)?)),
            Reference::Copied(copied) => Ok(Reference::Copied(f(copied)?)),
        }
    }

    pub fn get_ref<'a>(&self) -> &'a T
    where
        'b: 'a,
        'c: 'a,
    {
        match self {
            &Reference::Borrowed(borrowed) => borrowed,
            &Reference::Copied(copied) => copied,
        }
    }
}
