//! Internal stateless code for handling BSER deserialization.

use byteorder::{ByteOrder, NativeEndian};
use error_chain::bail;

use crate::de::read::{DeRead, Reference};
use crate::errors::*;
use crate::header::*;

#[derive(Debug)]
pub struct Bunser<R> {
    read: R,
    scratch: Vec<u8>,
}

pub struct PduInfo {
    pub bser_capabilities: u32,
    len: i64,
    start: i64,
}

impl<'de, R> Bunser<R>
where
    R: DeRead<'de>,
{
    pub fn new(read: R) -> Self {
        Bunser {
            read,
            scratch: Vec::with_capacity(128),
        }
    }

    /// Read the PDU off the stream. This should be called in the beginning.
    pub fn read_pdu(&mut self) -> Result<PduInfo> {
        {
            let magic = self.read_bytes(2)?;
            if magic.get_ref() != &EMPTY_HEADER[..2] {
                bail!("invalid magic header {:?}", magic);
            }
        }
        let bser_capabilities = self.read.next_u32(&mut self.scratch)?;
        let len = self.check_next_int()?;
        let start = self.read_count();
        Ok(PduInfo {
            bser_capabilities,
            len,
            start,
        })
    }

    pub fn read_count(&self) -> i64 {
        self.read.read_count() as i64
    }

    pub fn end(&self, pdu_info: &PduInfo) -> Result<()> {
        let expected = (pdu_info.start + pdu_info.len) as usize;
        if self.read.read_count() != expected {
            bail!(
                "Expected {} bytes read, but only read {} bytes",
                expected,
                self.read.read_count()
            );
        }
        Ok(())
    }

    #[inline]
    pub fn peek(&mut self) -> Result<u8> {
        self.read.peek()
    }

    #[inline]
    pub fn discard(&mut self) {
        self.read.discard();
    }

    /// Return a borrowed or copied version of the next n bytes.
    #[inline]
    pub fn read_bytes<'s>(&'s mut self, len: i64) -> Result<Reference<'de, 's, [u8]>> {
        let len = len as usize;
        self.read.next_bytes(len, &mut self.scratch)
    }

    /// Return the next i8 value. This assumes the caller already knows the next
    /// value is an i8.
    pub fn next_i8(&mut self) -> Result<i8> {
        self.read.discard();
        let bytes = self
            .read_bytes(1)
            .chain_err(|| "error while reading i8")?
            .get_ref();
        Ok(bytes[0] as i8)
    }

    /// Return the next i16 value. This assumes the caller already knows the
    /// next value is an i16.
    pub fn next_i16(&mut self) -> Result<i16> {
        self.read.discard();
        let bytes = self
            .read_bytes(2)
            .chain_err(|| "error while reading i16")?
            .get_ref();
        Ok(NativeEndian::read_i16(bytes))
    }

    /// Return the next i32 value. This assumes the caller already knows the
    /// next value is an i32.
    pub fn next_i32(&mut self) -> Result<i32> {
        self.read.discard();
        let bytes = self
            .read_bytes(4)
            .chain_err(|| "error while reading i32")?
            .get_ref();
        Ok(NativeEndian::read_i32(bytes))
    }

    /// Return the next i64 value. This assumes the caller already knows the
    /// next value is an i64.
    pub fn next_i64(&mut self) -> Result<i64> {
        self.read.discard();
        let bytes = self
            .read_bytes(8)
            .chain_err(|| "error while reading i64")?
            .get_ref();
        Ok(NativeEndian::read_i64(bytes))
    }

    /// Check and return the next integer value. Errors out if the next value is
    /// not actually an int.
    pub fn check_next_int(&mut self) -> Result<i64> {
        let value = match self.peek()? {
            BSER_INT8 => self.next_i8()? as i64,
            BSER_INT16 => self.next_i16()? as i64,
            BSER_INT32 => self.next_i32()? as i64,
            BSER_INT64 => self.next_i64()? as i64,
            ch => bail!(ErrorKind::DeInvalidStartByte("integer".into(), ch)),
        };

        Ok(value)
    }

    pub fn next_f64(&mut self) -> Result<f64> {
        self.read.discard();
        let bytes = self
            .read_bytes(8)
            .chain_err(|| "error while reading f64")?
            .get_ref();
        Ok(NativeEndian::read_f64(bytes))
    }
}
