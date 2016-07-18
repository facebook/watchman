/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
package com.facebook.watchman;

import java.io.InputStream;
import java.util.Map;

import com.google.common.base.Supplier;

class SerializedStreamMessageSupplier implements Supplier<Map<String, Object>> {
  private final Deserializer deserializer;
  private final InputStream stream;

  public SerializedStreamMessageSupplier(InputStream stream, Deserializer deserializer) {
    this.stream = stream;
    this.deserializer = deserializer;
  }

  @Override
  public Map<String, Object> get() {
    try {
      return deserializer.deserialize(stream);
    } catch (Exception e) {
      return null;
    }
  }
}
