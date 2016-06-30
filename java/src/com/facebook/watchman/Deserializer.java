/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 */
package com.facebook.watchman;

import java.io.IOException;
import java.io.InputStream;
import java.util.Map;

public interface Deserializer {

  /**
   * Reads the next object from the InputSteram, blocking until it becomes available.
   * @param stream the stream to read from
   * @return a deserialized object, read from the stream
   */
  Map<String, Object> deserialize(InputStream stream) throws IOException;
}
