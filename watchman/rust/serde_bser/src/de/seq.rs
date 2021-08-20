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

use serde::de;

use crate::errors::*;

use super::read::DeRead;
use super::reentrant::ReentrantGuard;
use super::Deserializer;

pub struct SeqAccess<'a, R> {
    de: &'a mut Deserializer<R>,
    remaining: usize,
}

impl<'a, 'de, R> SeqAccess<'a, R>
where
    R: 'a + DeRead<'de>,
{
    /// Create a new `MapAccess`.
    ///
    /// `_guard` makes sure the caller is accounting for the recursion limit.
    pub fn new(de: &'a mut Deserializer<R>, nitems: usize, _guard: &ReentrantGuard) -> Self {
        SeqAccess {
            de,
            remaining: nitems,
        }
    }
}

impl<'a, 'de, R> de::SeqAccess<'de> for SeqAccess<'a, R>
where
    R: 'a + DeRead<'de>,
{
    type Error = Error;

    fn next_element_seed<T>(&mut self, seed: T) -> Result<Option<T::Value>>
    where
        T: de::DeserializeSeed<'de>,
    {
        if self.remaining == 0 {
            Ok(None)
        } else {
            self.remaining -= 1;
            let value = seed.deserialize(&mut *self.de)?;
            Ok(Some(value))
        }
    }
}
