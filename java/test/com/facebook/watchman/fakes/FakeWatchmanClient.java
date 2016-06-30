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

package com.facebook.watchman.fakes;

import java.io.IOException;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

import com.facebook.watchman.Callback;
import com.facebook.watchman.WatchmanClient;

import com.google.common.util.concurrent.ListenableFuture;
import sun.reflect.generics.reflectiveObjects.NotImplementedException;

public class FakeWatchmanClient implements WatchmanClient {

  @Override
  public ListenableFuture<Map<String, Object>> clock(Path path) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> clock(Path path, Number syncTimeout) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> watch(Path path) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> watchDel(Path path) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Boolean> unsubscribe(SubscriptionDescriptor descriptor) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<SubscriptionDescriptor> subscribe(
      Path path, Map<String, Object> query, Callback listener) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> version() {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> version(
      List<String> optionalCapabilities, List<String> requiredCapabilities) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Map<String, Object>> run(List<Object> command) {
    throw new NotImplementedException();
  }

  @Override
  public ListenableFuture<Boolean> unsubscribeAll() {
    throw new NotImplementedException();
  }

  @Override
  public void close() throws IOException {
    throw new NotImplementedException();
  }

  @Override
  public void start() {
    throw new NotImplementedException();
  }
}
