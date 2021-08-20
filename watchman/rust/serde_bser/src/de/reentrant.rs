/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! Module to handle reentrant/recursion limits while deserializing.

use std::cell::Cell;
use std::rc::Rc;

use crate::errors::*;

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
            return Err(Error::DeRecursionLimitExceeded { kind: kind.into() });
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
