/*
 * Copyright 2015-present Facebook, Inc.
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

import java.io.IOException;
import java.util.List;
import java.util.Map;

import com.facebook.watchman.fakes.FakeWatchmanClient;

import com.google.common.collect.ImmutableMap;
import com.google.common.util.concurrent.ListenableFuture;
import com.google.common.util.concurrent.SettableFuture;
import org.junit.Assert;
import org.junit.Test;

public class CapabilitiesTest {
  public static String DEFAULT_VERSION_STRING = "1.2.3";

  /**
   * Make sure that CapabilitiesStrategy#checkWatchProjectCapability only calls WatchmanClient#version, and
   * no other method of the client. Also make sure that "cmd-watch-project" is the only required
   * capability, with no other optional capabilities being queried.
   */
  @Test
  public void testWatchProjectVersionCalled() {
    Map<String, Object> response = ImmutableMap.<String, Object>builder()
        .put("capabilities", ImmutableMap.builder().put("cmd-watch-project", true).build())
        .put("version", DEFAULT_VERSION_STRING)
        .build();
    final SettableFuture<Map<String, Object>> future = SettableFuture.create();
    future.set(response);

    WatchmanClient mock = new FakeWatchmanClient() {
      @Override
      public ListenableFuture<Map<String, Object>> version(
          List<String> optionalCapabilities,
          List<String> requiredCapabilities) {
        IllegalArgumentException e = new IllegalArgumentException(
            "The requiredCapabilities argument should be equal to [\"cmd-watch-project\"]");
        if (requiredCapabilities.size() != 1) throw e;
        if (!requiredCapabilities.get(0).equals("cmd-watch-project")) throw e;

        return future;
      }
    };
    CapabilitiesStrategy.checkWatchProjectCapability(mock);
  }

  /**
   * Test that the return value is correct, when Watchman supports capabilities, and watch-project
   * is one of them.
   */
  @Test
  public void testWatchProjectCapabilitySupported() {
    Map<String, Object> response = ImmutableMap.<String, Object>builder()
        .put("capabilities", ImmutableMap.builder().put("cmd-watch-project", true).build())
        .put("version", DEFAULT_VERSION_STRING)
        .build();
    final SettableFuture<Map<String, Object>> future = SettableFuture.create();
    future.set(response);

    WatchmanClient mock = new FakeWatchmanClient() {
      @Override
      public ListenableFuture<Map<String, Object>> version(
          List<String> optionalCapabilities,
          List<String> requiredCapabilities) {
        return future;
      }
    };
    Assert.assertTrue(CapabilitiesStrategy.checkWatchProjectCapability(mock));
  }

  /**
   * Test that the return value is correct, when watchman supports capabilities, but the
   * watch-project command is not supported. (theoretically impossible, since capabilities were
   * introduced in 3.8 and watch-project in 3.1, but better make sure).
   */
  @Test
  public void testWatchProjectCapabilityUnsupported() {
    final SettableFuture<Map<String, Object>> future = SettableFuture.create();
    future.setException(new WatchmanException("something"));

    WatchmanClient mock = new FakeWatchmanClient() {
      @Override
      public ListenableFuture<Map<String, Object>> version(
          List<String> optionalCapabilities,
          List<String> requiredCapabilities) {
        return future;
      }
    };
    Assert.assertFalse(CapabilitiesStrategy.checkWatchProjectCapability(mock));
  }

  /**
   * Add @Test to this method to check if the methods work fine with your local Watchman install.
   */
  @Test
  public void testIntegration() throws WatchmanSocketUnavailableException, IOException {
    for (int i = 0; i < 100; i++) {
      WatchmanClient client = new WatchmanClientImpl(WatchmanSocketBuilder.discoverSocket());
      client.start();
      Assert.assertTrue(CapabilitiesStrategy.checkWatchProjectCapability(client));
      client.close();
    }
  }

}
