/*
 * Copyright 2004-present Facebook. All Rights Reserved.
 */
package com.facebook.watchman;

import java.util.Collections;
import java.util.Map;
import java.util.concurrent.ExecutionException;

import com.google.common.util.concurrent.ListenableFuture;

/**
 * Called a "strategy" because we might have different ways of testing for capabilities as versions
 * change. Should become an interface once we get different implementations available.
 */
public class CapabilitiesStrategy {

  /**
   * Tests if a client supports the "watch-project" command or not.
   */
  public static boolean checkWatchProjectCapability(WatchmanClient client) {
    ListenableFuture<Map<String, Object>> future = client.version(
        Collections.<String>emptyList(),
        Collections.singletonList("cmd-watch-project"));
    try {
      Map<String, Object> response = future.get();
      return response.containsKey("capabilities");
    } catch (InterruptedException e) {
      return false;
    } catch (ExecutionException e) {
      return false;
    }
  }
}
