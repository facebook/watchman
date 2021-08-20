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
