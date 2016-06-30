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

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.WritableByteChannel;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Map;
import java.util.concurrent.TimeUnit;

import com.facebook.watchman.bser.BserDeserializer;
import com.facebook.watchman.environment.ExecutableFinder;
import com.facebook.watchman.unixsocket.UnixDomainSocket;

import com.google.common.base.Optional;
import com.google.common.collect.ImmutableMap;
import com.zaxxer.nuprocess.NuAbstractProcessHandler;
import com.zaxxer.nuprocess.NuProcess;
import com.zaxxer.nuprocess.NuProcessBuilder;

import static com.google.common.base.Preconditions.checkArgument;

public class WatchmanSocketBuilder {
  private static class BufferingOutputHandler extends NuAbstractProcessHandler {

    private final ByteArrayOutputStream buffer;
    private final WritableByteChannel sink;
    private Optional<IOException> throwable;
    private NuProcess process = null;

    public BufferingOutputHandler() {
      buffer = new ByteArrayOutputStream();
      sink = Channels.newChannel(buffer);
      throwable = Optional.absent();
    }

    @Override
    public void onStart(NuProcess nuProcess) {
      super.onStart(nuProcess);
      this.process = nuProcess;
    }

    @Override
    public void onStdout(ByteBuffer buffer, boolean closed) {
      try {
        sink.write(buffer);
        if (closed) {
          sink.close();
        }
      } catch (IOException e) {
        throwable = Optional.of(e);
        process.destroy(false);
      }
    }

    public ByteBuffer getOutput() throws IOException {
      if (throwable.isPresent()) {
        throw throwable.get();
      }
      return ByteBuffer.wrap(buffer.toByteArray());
    }
  }

  public static Socket discoverSocket() throws WatchmanSocketUnavailableException {
    return discoverSocket(0, TimeUnit.SECONDS); // forever
  }

  public static Socket discoverSocket(long duration, TimeUnit unit)
      throws WatchmanSocketUnavailableException {
    Optional<Path> optionalExecutable = ExecutableFinder.getOptionalExecutable(
        Paths.get("watchman"),
        ImmutableMap.copyOf(System.getenv()));

    if (!optionalExecutable.isPresent()) {
      throw new WatchmanSocketUnavailableException();
    }

    Path watchmanPath = optionalExecutable.get();
    NuProcessBuilder processBuilder = new NuProcessBuilder(
        watchmanPath.toString(),
        "--output-encoding=bser",
        "get-sockname");

    BufferingOutputHandler outputHandler = new BufferingOutputHandler();
    processBuilder.setProcessListener(outputHandler);
    NuProcess process = processBuilder.start();
    if (process == null) {
      throw new WatchmanSocketUnavailableException("Could not create process");
    }
    int exitCode;
    try {
      exitCode = process.waitFor(duration, unit);
    } catch (InterruptedException e) {
      throw new WatchmanSocketUnavailableException("Subprocess interrupted", e);
    }

    if (exitCode == Integer.MIN_VALUE) {
      throw new WatchmanSocketUnavailableException("Subprocess timed out");
    }
    if (exitCode != 0) {
      throw new WatchmanSocketUnavailableException(
          "Subprocess non-zero exit status: " + String.valueOf(exitCode));
    }

    ByteBuffer output;
    try {
      output = outputHandler.getOutput();
    } catch (IOException e) {
      throw new WatchmanSocketUnavailableException("Could not read subprocess output", e);
    }

    InputStream stream = new ByteArrayInputStream(output.array());
    BserDeserializer deserializer = new BserDeserializer(BserDeserializer.KeyOrdering.UNSORTED);
    Map<String, Object> deserializedValue = null;
    try {
      deserializedValue = deserializer.deserialize(stream);
    } catch (IOException e) {
      throw new WatchmanSocketUnavailableException("Could not deserialize BSER output", e);
    }

    checkArgument(deserializedValue.containsKey("sockname"));
    try {
      return UnixDomainSocket.createSocketWithPath(
          Paths.get(String.valueOf(deserializedValue.get("sockname"))));
    } catch (IOException e) {
      throw new WatchmanSocketUnavailableException("Could not create Unix socket to resulting path", e);
    }
  }

}
