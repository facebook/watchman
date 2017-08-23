//! Module to handle reentrant/recursion limits while deserializing.

use std::cell::Cell;
use std::rc::Rc;

use errors::*;

/// Sets a limit on the amount of recursion during deserialization. This does
/// not do any synchronization -- it is intended purely for single-threaded use.
pub struct ReentrantLimit(Rc<Cell<usize>>);

impl ReentrantLimit {
    /// Create a new reentrant limit.
    pub fn new(limit: usize) -> Self {
        ReentrantLimit(Rc::new(Cell::new(limit)))
    }

    /// Try to decrease the limit by 1. Return an RAII guard that when freed
    /// will increase the limit by 1.
    pub fn acquire<S: Into<String>>(&mut self, kind: S) -> Result<ReentrantGuard> {
        if self.0.get() == 0 {
            bail!(ErrorKind::DeRecursionLimitExceeded(kind.into()));
        }
        self.0.set(self.0.get() - 1);
        Ok(ReentrantGuard(self.0.clone()))
    }
}

/// RAII guard for reentrant limits.
pub struct ReentrantGuard(Rc<Cell<usize>>);

impl Drop for ReentrantGuard {
    fn drop(&mut self) {
        self.0.set(self.0.get() + 1);
    }
}
