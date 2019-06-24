/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */
include "eden/fs/service/eden.thrift"
namespace cpp2 facebook.eden

/** This file holds definitions for the streaming flavor of the Eden interface
 * This is only available to cpp2 clients and won't compile for other
 * language/runtimes. */

service StreamingEdenService extends eden.EdenService {
  /** Request notification about changes to the journal for
   * the specified mountPoint.
   * The JournalPosition at the time of the subscribe call
   * will be pushed to the client, and then each change will
   * be pushed to the client in near-real-time.
   * The client may then use methods like getFilesChangedSince()
   * to determine the precise nature of the changes.
   *
   * This is an implementation of the subscribe API using the
   * new rsocket based streaming thrift protocol.
   * The name is temporary: we want to make some API changes
   * but want to start pushing out an implementation now because
   * we've seen inflated memory usage for the older `subscribe`
   * method above. */
  stream eden.JournalPosition subscribeStreamTemporary(
    1: string mountPoint)
}
