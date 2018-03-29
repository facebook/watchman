use serde::de;

use errors::*;

use super::Deserializer;
use super::read::DeRead;
use super::reentrant::ReentrantGuard;

pub struct MapAccess<'a, R: 'a> {
    de: &'a mut Deserializer<R>,
    remaining: usize,
}

impl<'a, 'de, R> MapAccess<'a, R>
where
    R: 'a + DeRead<'de>,
{
    /// Create a new `MapAccess`.
    ///
    /// `_guard` makes sure the caller is accounting for the recursion limit.
    pub fn new(de: &'a mut Deserializer<R>, nitems: usize, _guard: &ReentrantGuard) -> Self {
        MapAccess {
            de: de,
            remaining: nitems,
        }
    }
}

impl<'a, 'de, R> de::MapAccess<'de> for MapAccess<'a, R>
where
    R: 'a + DeRead<'de>,
{
    type Error = Error;

    fn next_key_seed<K>(&mut self, seed: K) -> Result<Option<K::Value>>
    where
        K: de::DeserializeSeed<'de>,
    {
        if self.remaining == 0 {
            Ok(None)
        } else {
            self.remaining -= 1;
            // TODO: must only allow string keys
            let key = seed.deserialize(&mut *self.de)?;
            Ok(Some(key))
        }
    }

    fn next_value_seed<V>(&mut self, seed: V) -> Result<V::Value>
    where
        V: de::DeserializeSeed<'de>,
    {
        seed.deserialize(&mut *self.de)
    }
}
