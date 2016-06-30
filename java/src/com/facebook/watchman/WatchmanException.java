/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 */
package com.facebook.watchman;

import java.util.Map;

public class WatchmanException extends Exception {

  private final Map<String, Object> response;

  public WatchmanException() {
    super();
    response = null;
  }

  public WatchmanException(String reason) {
    super(reason);
    response = null;
  }

  public WatchmanException(String error, Map<String, Object> response) {
    super(error);
    this.response = response;
  }

  public Map<String, Object> getResponse() {
    return response;
  }
}
