/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 */
package com.facebook.watchman;

import java.util.Map;

public interface Callback {
  void call(Map<String, Object> message) throws Exception;
}
