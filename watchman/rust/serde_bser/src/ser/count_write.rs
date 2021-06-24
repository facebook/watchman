use std::io::{self, Write};

/// A writer that counts how many bytes were written.
pub struct CountWrite {
    count: usize,
}

impl CountWrite {
    pub fn new() -> Self {
        CountWrite { count: 0 }
    }

    #[inline]
    pub fn count(&self) -> usize {
        self.count
    }
}

impl Write for CountWrite {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.count += buf.len();
        Ok(buf.len())
    }

    #[inline]
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
