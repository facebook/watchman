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
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Collection;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

import com.facebook.watchman.bser.BserDeserializer;
import com.facebook.watchman.bser.BserSerializer;

import com.google.common.base.Optional;
import com.google.common.collect.Queues;
import com.google.common.util.concurrent.ListenableFuture;
import com.google.common.util.concurrent.ListeningExecutorService;
import com.google.common.util.concurrent.MoreExecutors;
import com.google.common.util.concurrent.SettableFuture;
import com.google.common.util.concurrent.ThreadFactoryBuilder;
import org.immutables.value.Value;

public class WatchmanConnection {

  private final ListeningExecutorService outgoingMessageExecutor;
  private final ExecutorService incomingMessageExecutor;
  private final Callable<Map<String, Object>> incomingMessageGetter;
  private final Optional<WatchmanTransport> socket;
  private final OutputStream outgoingMessageStream;
  private final Optional<Callback> unilateralCallback;
  private final Optional<Collection<String>> unilateralLabels;
  private final BlockingQueue<QueuedCommand> commandQueue;
  private final AtomicBoolean processing;
  private final BserSerializer bserSerializer;
  private final Optional<WatchmanCommandListener> commandListener;

  public WatchmanConnection(WatchmanTransport socket) throws IOException {
    this(
        incomingMessageGetterFromSocket(socket),
        socket.getOutputStream(),
        Optional.<Collection<String>>absent(),
        Optional.<Callback>absent(),
        Optional.<WatchmanCommandListener>absent(),
        Optional.<WatchmanTransport>of(socket));
  }

  public WatchmanConnection(
      final WatchmanTransport socket,
      Optional<Collection<String>> unilateralLabels,
      Optional<Callback> unilateralCallback) throws IOException {
    this(
        incomingMessageGetterFromSocket(socket),
        socket.getOutputStream(),
        unilateralLabels,
        unilateralCallback,
        Optional.<WatchmanCommandListener>absent(),
        Optional.<WatchmanTransport>of(socket));
  }

  public WatchmanConnection(
      final WatchmanTransport socket,
      Optional<Collection<String>> unilateralLabels,
      Optional<Callback> unilateralCallback,
      Optional<WatchmanCommandListener> commandListener) throws IOException {
    this(
        incomingMessageGetterFromSocket(socket),
        socket.getOutputStream(),
        unilateralLabels,
        unilateralCallback,
        commandListener,
        Optional.<WatchmanTransport>of(socket));
  }

  public WatchmanConnection(
      Callable<Map<String, Object>> incomingMessageGetter,
      OutputStream outgoingMessageStream) {
    this(
        incomingMessageGetter,
        outgoingMessageStream,
        Optional.<Collection<String>>absent(),
        Optional.<Callback>absent(),
        Optional.<WatchmanCommandListener>absent(),
        Optional.<WatchmanTransport>absent());
  }

  public WatchmanConnection(
      Callable<Map<String, Object>> incomingMessageGetter,
      OutputStream outgoingMessageStream,
      Optional<WatchmanCommandListener> commandListener) {
    this(
        incomingMessageGetter,
        outgoingMessageStream,
        Optional.<Collection<String>>absent(),
        Optional.<Callback>absent(),
        commandListener,
        Optional.<WatchmanTransport>absent());
  }

  public WatchmanConnection(
      Callable<Map<String, Object>> incomingMessageGetter,
      OutputStream outgoingMessageStream,
      Optional<Collection<String>> unilateralLabels,
      Optional<Callback> unilateralCallback) {
    this(
        incomingMessageGetter,
        outgoingMessageStream,
        unilateralLabels,
        unilateralCallback,
        Optional.<WatchmanCommandListener>absent(),
        Optional.<WatchmanTransport>absent());
  }

  public WatchmanConnection(
      Callable<Map<String, Object>> incomingMessageGetter,
      OutputStream outgoingMessageStream,
      Optional<Collection<String>> unilateralLabels,
      Optional<Callback> unilateralCallback,
      Optional<WatchmanCommandListener> commandListener,
      Optional<WatchmanTransport> optionalSocket) {
    this.incomingMessageGetter = incomingMessageGetter;
    this.outgoingMessageStream = outgoingMessageStream;
    this.unilateralLabels = unilateralLabels;
    this.unilateralCallback = unilateralCallback;
    this.socket = optionalSocket;
    this.processing = new AtomicBoolean(true);
    this.outgoingMessageExecutor = MoreExecutors.listeningDecorator(
        Executors.newSingleThreadExecutor(
            new ThreadFactoryBuilder()
                .setNameFormat("[watchman] Outgoing Message Executor")
                .build()));
    this.commandQueue = Queues.newLinkedBlockingDeque();
    this.bserSerializer = new BserSerializer();
    this.commandListener = commandListener;
    this.incomingMessageExecutor = Executors.newSingleThreadExecutor(
        new ThreadFactoryBuilder()
            .setNameFormat("[watchman] Incoming Message Executor")
            .build());
  }

  private boolean checkMessageUnilateral(Map<String, Object> response) {
    if (! unilateralLabels.isPresent()) return false;
    for (String label: unilateralLabels.get()) {
      if (response.containsKey(label)) return true;
    }
    return false;
  }

  public ListenableFuture<Map<String, Object>> run(final Object command) {
    if (! processing.get()) {
      SettableFuture<Map<String, Object>> die = SettableFuture.create();
      die.setException(new WatchmanException("connection closing down"));
      return die;
    }
    final CountDownLatch latch = new CountDownLatch(1);
    final AtomicReference<Map<String, Object>> resultRef =
        new AtomicReference<Map<String, Object>>();
    final AtomicReference<Exception> errorRef = new AtomicReference<Exception>();
    QueuedCommand queuedCommand = new QueuedCommandBuilder()
        .command(command)
        .latch(latch)
        .resultRef(resultRef)
        .errorRef(errorRef)
        .build();
    commandQueue.add(queuedCommand);
    return outgoingMessageExecutor.submit(new Callable<Map<String, Object>>() {
      @Override
      public Map<String, Object> call() throws Exception {
        if (commandListener.isPresent()) {
          commandListener.get().onStart();
        }
        if (processing.get()) {
          bserSerializer.serializeToStream(
              command,
              outgoingMessageStream);
        }
        if (commandListener.isPresent()) {
          commandListener.get().onSent();
        }
        latch.await();
        if (commandListener.isPresent()) {
          commandListener.get().onReceived();
        }
        if (resultRef.get() != null) return resultRef.get();
        throw errorRef.get();
      }
    });
  }

  private void failAllCommands(Exception e) {
    processing.set(false);
    for (QueuedCommand command: commandQueue) {
      command.errorRef().set(e);
      command.latch().countDown();
    }
  }

  public void close() throws IOException {
    failAllCommands(new WatchmanException("connection closing down"));
    outgoingMessageStream.close();
    incomingMessageExecutor.shutdown();
    outgoingMessageExecutor.shutdown();

    if (socket.isPresent()) {
      socket.get().close();
    }
  }

  public void start() {
    this.incomingMessageExecutor.execute(new IncomingMessageThread());
  }

  private class IncomingMessageThread implements Runnable {

    @Override
    public void run() {
      while (processing.get()) {
        try {
          Map<String, Object> deserializedResponse = incomingMessageGetter.call();
          if (deserializedResponse == null) continue;

          if (checkMessageUnilateral(deserializedResponse)) {
            if (unilateralCallback.isPresent()) {
              unilateralCallback.get().call(deserializedResponse);
            } else {
              failAllCommands(
                  new Exception("Received unilateral message without any callback registered"));
              return;
            }
            continue;
          }

          QueuedCommand lastCommand = commandQueue.take();
          if (deserializedResponse.containsKey("error")) {
            lastCommand.errorRef().set(new WatchmanException(
                String.valueOf(deserializedResponse.get("error")),
                deserializedResponse));
          } else {
            lastCommand.resultRef().set(deserializedResponse);
          }
          lastCommand.latch().countDown();
        } catch (Exception e) {
          failAllCommands(e);
          return;
        }
      }
    }
  }

  @Value.Immutable
  @Value.Style(visibility = Value.Style.ImplementationVisibility.PRIVATE)
  interface QueuedCommand {
    Object command();
    CountDownLatch latch();
    AtomicReference<Map<String, Object>> resultRef();
    AtomicReference<Exception> errorRef();
  }

  /**
   * Permits the synchronization of test classes with the thread sending messages. If the
   * WatchmanConnection has a WatchmanCommandListener attached, it will make sure that the methods
   * will be called:
   * <ul>
   *   <li>onStart: when the thread picks up the command</li>
   *   <li>onSent: when the serialization to the OutputStream is done</li>
   *   <li>onReceived: when a response from Watchman is received</li>
   * </ul>
   */
  public interface WatchmanCommandListener {
    void onStart();
    void onSent();
    void onReceived();
  }

  /**
   * Generates a Callable that can be invoked repeatedly to extract
   * deserialized messages from watchman's socket.
   */
  private static Callable<Map<String, Object>> incomingMessageGetterFromSocket(WatchmanTransport socket)
      throws IOException {
    final InputStream inputStream = socket.getInputStream();
    final BserDeserializer deserializer = new BserDeserializer(BserDeserializer.KeyOrdering.UNSORTED);
    return new Callable<Map<String, Object>>() {
      @Override
      public Map<String, Object> call() throws Exception {
        return deserializer.deserialize(inputStream);
      }
    };
  }
}
