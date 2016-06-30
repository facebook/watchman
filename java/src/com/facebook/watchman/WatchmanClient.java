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

import java.io.IOException;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.Objects;

import com.google.common.util.concurrent.ListenableFuture;
import org.immutables.value.Value;

public interface WatchmanClient {

  ListenableFuture<Map<String, Object>> clock(Path path);
  ListenableFuture<Map<String, Object>> clock(Path path, Number syncTimeout);

  ListenableFuture<Map<String, Object>> watch(Path path);

  ListenableFuture<Map<String,Object>> watchDel(Path path);

  ListenableFuture<Boolean> unsubscribe(SubscriptionDescriptor descriptor);

  ListenableFuture<SubscriptionDescriptor> subscribe(
      Path path,
      Map<String, Object> query,
      Callback listener);

  ListenableFuture<Map<String, Object>> version();

  ListenableFuture<Map<String, Object>> version(
      List<String> optionalCapabilities,
      List<String> requiredCapabilities);

  ListenableFuture<Map<String, Object>> run(List<Object> command);

  ListenableFuture<Boolean> unsubscribeAll();

  void close() throws IOException;

  void start();

  @Value.Immutable
  @Value.Style(visibility = Value.Style.ImplementationVisibility.PRIVATE)
  abstract class SubscriptionDescriptor {
    public abstract String root();
    public abstract String name();

    @Override
    public int hashCode() {
      return Objects.hash(root(), name());
    }
  }
}
