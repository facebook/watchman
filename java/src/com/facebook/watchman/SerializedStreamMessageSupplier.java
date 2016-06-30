/*
 * Copyright 2004-present Facebook. All Rights Reserved.
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
